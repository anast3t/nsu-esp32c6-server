/* ------------------------------------------------------------------
 *  ESP32-C6 SoftAP + TCP server
 * ----------------------------------------------------------------- */
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "driver/gpio.h"
#include "led_strip.h"
#include "esp_cpu.h"
#include "esp_system.h"

#define MY_AP_SSID  "ESP32C6_AP"
#define MY_AP_PASS  "12345678"
#define TCP_PORT    5000
#define BOOT_BTN    9
#define LED_GPIO    8
#define LED_COUNT   1

static const char *TAG = "SOFTAP";
static led_strip_handle_t strip;
static TaskHandle_t led_task = NULL;   /* <-- кому слать уведомление */
static int client_fd = -1;
static volatile uint32_t t_cycle = 0;

/* ---------- Wi-Fi 4 / 6 ------------------------------------------------- */
typedef enum { WIFI_STD_4, WIFI_STD_6 } wifi_std_t;
static esp_err_t wifi_ap_set_proto(wifi_std_t s)
{
    wifi_protocols_t p = {0};
    p.ghz_2g = (s == WIFI_STD_4)
               ? (WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N)
               :  WIFI_PROTOCOL_11AX;
    return esp_wifi_set_protocols(WIFI_IF_AP, &p);
}

/* ---------- ISR --------------------------------------------------------- */
static void IRAM_ATTR btn_isr(void *arg)
{
    t_cycle = esp_cpu_get_cycle_count();
    BaseType_t hp = pdFALSE;
    vTaskNotifyGiveFromISR(led_task, &hp);
    if (hp) portYIELD_FROM_ISR();
}

/* ---------- TCP server task -------------------------------------------- */
static void tcp_srv_task(void *arg)
{
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET,
                             .sin_port   = htons(TCP_PORT),
                             .sin_addr.s_addr = htonl(INADDR_ANY) };
    bind(srv, (void*)&a, sizeof(a));
    listen(srv, 1);
    ESP_LOGI(TAG, "TCP server: %d", TCP_PORT);

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

/* ---------- LED task ---------------------------------------------------- */
static void led_task_fn(void *arg)
{
    bool on = false;
    const uint32_t cpu = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000UL;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        led_strip_set_pixel(strip, 0, on ? 32 : 0, 0, 0);
        led_strip_refresh(strip);
        if (client_fd >= 0) {
            char buf[96];
            int n = snprintf(buf, sizeof(buf),
                             "LED:%s\n",
                             on ? "ON" : "OFF");
            send(client_fd, buf, n, 0);
        }
        uint32_t d = esp_cpu_get_cycle_count() - t_cycle;
        float ns = d * 1e9f / cpu, ms = ns / 1e6f;
        ESP_LOGI(TAG, "LED %s | cycles:%lu %.2f ns (%.6f ms)",
                 on ? "ON" : "OFF", d, ns, ms);
        on = !on;
    }
}

/* ---------- Wi-Fi init -------------------------------------------------- */
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
    wifi_ap_set_proto(WIFI_STD_6);      /* Wi-Fi 6 only */
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_start();

    wifi_protocols_t chk;
    esp_wifi_get_protocols(WIFI_IF_AP, &chk);
    ESP_LOGI(TAG, "AP proto2G=0x%X (0x20=AX)", chk.ghz_2g);
}

/* ---------- app_main ---------------------------------------------------- */
void app_main(void)
{
    nvs_flash_init(); wifi_init();

    /* LED-strip */
    led_strip_config_t c = { .strip_gpio_num = LED_GPIO,
                             .max_leds       = LED_COUNT,
                             .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
                             .led_model      = LED_MODEL_WS2812 };
    led_strip_rmt_config_t r = { .clk_src = RMT_CLK_SRC_DEFAULT,
                                 .resolution_hz = 10 * 1000 * 1000 };
    led_strip_new_rmt_device(&c, &r, &strip);

    /* GPIO и ISR */
    gpio_config_t b = { .pin_bit_mask = 1ULL << BOOT_BTN,
                        .mode = GPIO_MODE_INPUT,
                        .pull_up_en = 1,
                        .intr_type = GPIO_INTR_NEGEDGE };
    gpio_config(&b);
    gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    gpio_isr_handler_add(BOOT_BTN, btn_isr, NULL);

    /* Задачи */
    xTaskCreatePinnedToCore(led_task_fn, "led", 2048, NULL, 10, &led_task, 0);
    xTaskCreatePinnedToCore(tcp_srv_task, "srv", 4096, NULL, 8, NULL, 0);
}
