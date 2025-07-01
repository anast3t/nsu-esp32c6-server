/* ------------------------------------------------------------------
 *  ESP32-C6  ―  Wi-Fi AP + TCP  ‖  BLE Peripheral + GATT Notify
 *  переключается строкой PROTO_BT
 * ----------------------------------------------------------------- */
#define PROTO_BT   0            /* 0 = Wi-Fi,  1 = Bluetooth LE */

/* ---------- общие incluye ----------------------------------------- */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "led_strip.h"
#include "esp_cpu.h"

/* ---------- BLE ---------------------------------------------------- */
#if PROTO_BT
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "esp_bt.h"
#else            /* ---------- Wi-Fi / TCP --------------------------- */
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#endif

/* ---------- конфигурация ------------------------------------------ */
#define BOOT_BTN     9
#define LED_GPIO     8
#define LED_COUNT    1
#define TAG          "LAT_SRV"

#if !PROTO_BT
#define MY_AP_SSID   "ESP32C6_AP"
#define MY_AP_PASS   "12345678"
#define TCP_PORT     5000
#endif

static led_strip_handle_t strip;
static TaskHandle_t        led_task;
static volatile uint32_t   t_cycle;
static bool     led_on = false;

/* ************************************************************************** */
/*                              TRANSPORT-INDEPENDENT                         */
/* ************************************************************************** */
static void IRAM_ATTR btn_isr(void *arg)
{
    t_cycle = esp_cpu_get_cycle_count();
    BaseType_t hp = pdFALSE;
    vTaskNotifyGiveFromISR(led_task, &hp);
    if (hp) portYIELD_FROM_ISR();
}


/* ************************************************************************** */
/*                              Wi-Fi / TCP PART                              */
/* ************************************************************************** */
#if !PROTO_BT
static int client_fd = -1;
typedef enum { WIFI_STD_4, WIFI_STD_6 } wifi_std_t;
static esp_err_t wifi_ap_set_proto(wifi_std_t s)
{
    wifi_protocols_t p = {0};
    p.ghz_2g = (s == WIFI_STD_4)
               ? (WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N)
               :  WIFI_PROTOCOL_11AX;
    return esp_wifi_set_protocols(WIFI_IF_AP, &p);
}

static void wifi_init(void)
{
    esp_netif_init();  esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t ap = { .ap = {
            .ssid = MY_AP_SSID, .password = MY_AP_PASS,
            .authmode = WIFI_AUTH_WPA2_PSK, .max_connection = 4, .channel = 1 }};
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    wifi_ap_set_proto(WIFI_STD_6);
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_start();
}

static void tcp_srv_task(void *arg)
{
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in a = { .sin_family = AF_INET,
                             .sin_port   = htons(TCP_PORT),
                             .sin_addr.s_addr = htonl(INADDR_ANY) };
    bind(srv, (void*)&a, sizeof(a));  listen(srv, 1);
    ESP_LOGI(TAG, "TCP server %d ready", TCP_PORT);

    for (;;) {
        struct sockaddr_in cli; socklen_t len = sizeof(cli);
        int fd = accept(srv, (void*)&cli, &len);
        if (fd >= 0) {
            if (client_fd >= 0) close(client_fd);
            client_fd = fd;
            ESP_LOGI(TAG, "Client connected");
        }
    }
}
#endif /* !PROTO_BT */

/* ===================================================================== *
 *  BLE-инициализация  (NimBLE Peripheral, имя  ESP32C6_LED)              *
 *  — сервис 0x1815, characteristic 0x2A56 с READ + NOTIFY               *
 * ===================================================================== */
#if PROTO_BT

#define BLE_SVC_UUID16  0x1815
#define BLE_CHR_UUID16  0x2A56

static uint16_t gatt_chr_handle;               /* h-val для notify   */
static uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint8_t  own_addr_type;                 /* сохраняем при sync */

/* ───────── GATT: callback «прочитать» ─────────────────────────────── */
/* GATT read = вернуть “ON/OFF” */
static int gatt_access_cb(uint16_t conn, uint16_t attr,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    const char *s = led_on ? "LED:ON" : "LED:OFF";
    return os_mbuf_append(ctxt->om, s, strlen(s));
}

/* Service 0x1815 / Char 0x2A56 */
static const struct ble_gatt_svc_def gatt_svcs[] = {{
    .type = BLE_GATT_SVC_TYPE_PRIMARY,
    .uuid = BLE_UUID16_DECLARE(0x1815),
    .characteristics = (struct ble_gatt_chr_def[]) {{
        .uuid       = BLE_UUID16_DECLARE(0x2A56),
        .access_cb  = gatt_access_cb,
        .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &gatt_chr_handle,
    }, {0}}
}, {0}};

/* ------------------------------------------------------------------ */
/*  GAP-callback: обрабатывает CONNECT, DISCONNECT и SUBSCRIBE        */
/* ------------------------------------------------------------------ */
static int gap_cb(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {

    /* ---------- установлено / не удалось соединение ---------------- */
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            conn_handle = ev->connect.conn_handle;
            ESP_LOGI(TAG, "BLE connected (handle=%d)", conn_handle);
        } else {
            ESP_LOGW(TAG, "Connect failed (status=%d) – restart ADV",
                     ev->connect.status);
            ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                              &((struct ble_gap_adv_params){
                                  .conn_mode = BLE_GAP_CONN_MODE_UND,
                                  .disc_mode = BLE_GAP_DISC_MODE_GEN }),
                              gap_cb, NULL);
        }
        break;

    /* ---------- разрыв соединения ---------------------------------- */
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnect (reason=%d)",
                 ev->disconnect.reason);
        conn_handle = BLE_HS_CONN_HANDLE_NONE;
        ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                          &((struct ble_gap_adv_params){
                              .conn_mode = BLE_GAP_CONN_MODE_UND,
                              .disc_mode = BLE_GAP_DISC_MODE_GEN }),
                          gap_cb, NULL);
        break;

    /* ---------- клиент (de)подписался на notify/indicate ----------- */
    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG,
                 "SUBSCRIBE attr=0x%04x  notify=%d  indicate=%d",
                 ev->subscribe.attr_handle,
                 ev->subscribe.cur_notify,
                 ev->subscribe.cur_indicate);
        break;

    default:
        break;
    }
    return 0;
}


/* sync → настраиваем всё и стартуем рекламу */
static void ble_app_on_sync(void)
{
    ble_hs_id_infer_auto(0, &own_addr_type);
    if (own_addr_type != BLE_ADDR_PUBLIC) own_addr_type = BLE_ADDR_PUBLIC; /* <─ */

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(gatt_svcs);
    ble_gatts_add_svcs(gatt_svcs);
    ble_gatts_start();  
    ble_svc_gap_device_name_set("ESP32C6_LED");

    static const ble_uuid16_t svc_uuid = BLE_UUID16_INIT(0x1815);
    struct ble_hs_adv_fields adv = {0};
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.uuids16 = &svc_uuid; adv.num_uuids16 = 1; adv.uuids16_is_complete = 1;
    ble_gap_adv_set_fields(&adv);

    struct ble_hs_adv_fields rsp = {0};
    const char *nm = "ESP32C6_LED";
    rsp.name = (const uint8_t*)nm; rsp.name_len = strlen(nm); rsp.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);

    struct ble_gap_adv_params p = { .conn_mode = BLE_GAP_CONN_MODE_UND,
                                    .disc_mode = BLE_GAP_DISC_MODE_GEN };
    ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_cb, NULL);
}

/* ───────── NimBLE task-обёртка (остается в цикле) ────────────────── */
static void nimble_host_task(void *param)   /* ← любое имя */
{
    nimble_port_run();          /* никогда не выходит */
    nimble_port_freertos_deinit();
}

/* ───────── инициализация BLE-стека ───────────────────────────────── */
static void ble_init(void)
{
    esp_bt_mem_release(ESP_BT_MODE_CLASSIC_BT);   /* освободить BT Classic RAM */
    nimble_port_init();
    ble_hs_cfg.sync_cb = ble_app_on_sync;

    /* передаём **указатель на функцию**, без [] и {} */
    nimble_port_freertos_init(nimble_host_task);
}
#endif /* PROTO_BT */


#if PROTO_BT
/* ------------ BLE Notify ------------------------------------------ */

static int tx_send(const char *msg, size_t len) /* возвращаем rc */
{
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE){/* нет соединения */
        // ESP_LOGW(TAG, "TX_send warns: no connection");
        return BLE_HS_ENOTCONN;
    }   

    /* защита от слишком длинного сообщения (MTU-3)            *
     * 23-байт MTU по умолчанию → 20 байт доступно в ATT Value */
    if (len > BLE_ATT_MTU_DFLT - 3) len = BLE_ATT_MTU_DFLT - 3;

    struct os_mbuf *om = ble_hs_mbuf_from_flat(msg, len);
    int rc = ble_gatts_notify_custom(conn_handle, gatt_chr_handle, om);

    if (rc != 0) {                        /* если стек вернул ошибку   */
        os_mbuf_free_chain(om);           /* — освобождаем буфер сами  */
        ESP_LOGW(TAG, "Notify error rc=%d", rc);
    }
    return rc;
}
#else
/* ------------ TCP Send -------------------------------------------- */
static int tx_send(const char *msg, size_t len)
{
    return (client_fd >= 0) ? send(client_fd, msg, len, 0) : -1;
}
#endif


/* ************************************************************************** */
/*                               LED-TASK                                     */
/* ************************************************************************** */
static void led_task_fn(void *arg)
{
    bool on = false;
    const uint32_t cpu = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000UL;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        led_strip_set_pixel(strip, 0, on ? 32 : 0, 0, 0);
        led_strip_refresh(strip);

        led_on = on;
        char buf[16];
        int  n = snprintf(buf, sizeof(buf), "LED:%s\n", on ? "ON" : "OFF");
        tx_send(buf, n);

        uint32_t d  = esp_cpu_get_cycle_count() - t_cycle;
        float    ns = d * 1e9f / cpu;
        ESP_LOGI(TAG, "LED %s | %d cycles | %.3f µs | %.3f ms", on ? "ON" : "OFF", d, ns / 1e3, ns / 1e6);
        on = !on;
    }
}

/* ************************************************************************** */
/*                                MAIN                                        */
/* ************************************************************************** */
void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    esp_log_level_set("NimBLE", ESP_LOG_NONE);

#if PROTO_BT
    ble_init();
#else
    wifi_init();
    xTaskCreatePinnedToCore(tcp_srv_task, "tcp", 4096, NULL, 8, NULL, 0);
#endif

    /* LED-strip init ---------------------------------------------------- */
    led_strip_config_t cfg = {
        .strip_gpio_num = LED_GPIO,
        .max_leds       = LED_COUNT,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .led_model      = LED_MODEL_WS2812
    };
    led_strip_rmt_config_t rmt = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000
    };
    led_strip_new_rmt_device(&cfg, &rmt, &strip);

    /* кнопка + ISR ------------------------------------------------------ */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_BTN,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = 1,
        .intr_type    = GPIO_INTR_NEGEDGE
    };
    gpio_config(&io);
    gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    gpio_isr_handler_add(BOOT_BTN, btn_isr, NULL);

    /* LED-task ---------------------------------------------------------- */
    xTaskCreatePinnedToCore(led_task_fn, "led", 2048, NULL, 10, &led_task, 0);
}
