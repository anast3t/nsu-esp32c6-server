#include "transport.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "esp_bt.h"

#define BLE_SVC_UUID16  0x1815
#define BLE_CHR_UUID16  0x2A56

static const char *TAG = "TRANSPORT_BLE";

static uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t gatt_chr_handle;               /* h-val для notify   */
static uint8_t  own_addr_type;                 /* сохраняем при sync */

/* ───────── GATT: callback «прочитать» ─────────────────────────────── */
/* GATT read = вернуть “ON/OFF” */
static int gatt_access_cb(uint16_t conn, uint16_t attr,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    const char *s = led_ctrl_is_on() ? "LED:ON" : "LED:OFF";
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
esp_err_t transport_init(void) {
    // стек NimBLE
    esp_bt_mem_release(ESP_BT_MODE_CLASSIC_BT);
    nimble_port_init();
    ble_hs_cfg.sync_cb = ble_app_on_sync;
    nimble_port_freertos_init(nimble_host_task);
    ESP_LOGI("TRANSPORT_BLE", "BLE transport ready");
    return ESP_OK;
}

int transport_send(const char *msg, size_t len) {
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