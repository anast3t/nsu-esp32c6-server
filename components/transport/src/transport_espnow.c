// transport_espnow_server.c
#include "transport.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <string.h>

static const char *TAG = "ESPNOW_SRV";

/* ======= ПАРАМЕТРЫ ======= */
#define ESPNOW_CHANNEL            5
#define ESPNOW_MAX_PAYLOAD        250
#define MAX_INFLIGHT_FRAMES       6      // ограничение кадров «в полёте»
#define TX_TASK_STACK             3072
#define TX_TASK_PRIO              9

// MAC клиента-приёмника (ВАШ)
static const uint8_t PEER_MAC[6] = { 0x40,0x4c,0xca,0x55,0xd5,0x10 };

/* Шифрование: AES-CCM (per-peer). Broadcast не шифруется. */
#define ESPNOW_ENCRYPT            0
#if ESPNOW_ENCRYPT
static const uint8_t PMK[16] = "0123456789ABCDEF";  // 16 байт
static const uint8_t LMK[16] = "C6_LINK_KEY_0001";  // 16 байт (пара устройств)
#endif

/* ======= БЭКПРЕШЕР: очередь «последнего» и лимит inflight ======= */
typedef struct {
    uint16_t len;
    uint8_t  data[ESPNOW_MAX_PAYLOAD];
} tx_item_t;

static QueueHandle_t s_txq;          // длина 1 → храним только ПОСЛЕДНЕЕ сообщение
static TaskHandle_t  s_tx_task = NULL;
static volatile int  s_inflight = 0;

/* on-send: уменьшаем inflight и будим воркер */
static void on_espnow_sent(const uint8_t *mac, esp_now_send_status_t status)
{
    (void)mac; (void)status;
    if (s_inflight > 0) s_inflight--;
    BaseType_t hp = pdFALSE;
    if (s_tx_task) vTaskNotifyGiveFromISR(s_tx_task, &hp);
    if (hp) portYIELD_FROM_ISR();
}

/* воркер отправки: уважает inflight и NO_MEM */
static void tx_worker(void *arg)
{
    tx_item_t item;
    for (;;) {
        if (xQueueReceive(s_txq, &item, portMAX_DELAY) != pdTRUE) continue;

        for (;;) {
            while (s_inflight >= MAX_INFLIGHT_FRAMES) {
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            }
            esp_err_t rc = esp_now_send(PEER_MAC, item.data, item.len);
            if (rc == ESP_OK) { s_inflight++; break; }
            if (rc == ESP_ERR_ESPNOW_NO_MEM) {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1));
                continue;
            }
            ESP_LOGW(TAG, "esp_now_send rc=%s", esp_err_to_name(rc));
            break;
        }
    }
}

/* попытка задать GN-only; если недоступно — откатываемся на BGN */
static void wifi_set_protocols_sta(void)
{
    // Новый API (ESP-IDF 5.x): per-band bitmap
    wifi_protocols_t p = {0};
    p.ghz_2g = WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N;
    esp_err_t rc = esp_wifi_set_protocols(WIFI_IF_STA, &p);
    if (rc == ESP_ERR_INVALID_ARG || rc == ESP_ERR_NOT_SUPPORTED) {
        // fallback: BGN
        p.ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N;
        ESP_ERROR_CHECK(esp_wifi_set_protocols(WIFI_IF_STA, &p));
        ESP_LOGW(TAG, "GN-only not supported for STA on this build → fallback to BGN");
    }
}

/* per-peer установка PHY (HE/HT/54M) с безопасными фолбэками */
static void set_peer_rate_config(const uint8_t mac[6])
{
// #if defined(esp_now_set_peer_rate_config)
    esp_now_rate_config_t rcfg = {0};

    // Режим канала
    // #ifdef WIFI_PHY_MODE_HE20
    // rcfg.phymode = WIFI_PHY_MODE_HE20;        // 802.11ax 20 MHz
    // #elif defined(WIFI_PHY_MODE_HT20)
    rcfg.phymode = WIFI_PHY_MODE_HT20;        // 802.11n 20 MHz
    // #else
    // rcfg.phymode = WIFI_PHY_MODE_11G;         // fallback
    // #endif

    // Скорость
    // #ifdef WIFI_PHY_RATE_HE_MCS7_SGI
    // rcfg.rate = WIFI_PHY_RATE_HE_MCS7_SGI;    // HE + SGI (высокая)
    // #elif defined(WIFI_PHY_RATE_MCS7_SGI)
    rcfg.rate = WIFI_PHY_RATE_MCS7_SGI;       // HT MCS7 + SGI
    // #else
    // rcfg.rate = WIFI_PHY_RATE_54M;            // OFDM 54M (g)
    // #endif

    (void)esp_now_set_peer_rate_config(mac, &rcfg);
// #endif
}

/* ======= ПУБЛИЧНЫЙ API ======= */

esp_err_t transport_init(void)
{
    /* Netif + Wi-Fi STA */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));
    wifi_set_protocols_sta();

    // (опц.) мощность TX
    // ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(84)); // ~21 dBm, если допустимо

    // лог местного MAC
    uint8_t self_mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, self_mac);
    ESP_LOGI(TAG, "STA up (chan=%d, MAC=%02X:%02X:%02X:%02X:%02X:%02X)",
             ESPNOW_CHANNEL,
             self_mac[0],self_mac[1],self_mac[2],self_mac[3],self_mac[4],self_mac[5]);

    /* ESP-NOW */
    ESP_ERROR_CHECK(esp_now_init());
#if ESPNOW_ENCRYPT
    ESP_ERROR_CHECK(esp_now_set_pmk(PMK));
#endif
    esp_now_register_send_cb(on_espnow_sent);

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, PEER_MAC, 6);
    peer.channel = ESPNOW_CHANNEL;
    peer.ifidx   = ESP_IF_WIFI_STA;
#if ESPNOW_ENCRYPT
    peer.encrypt = true;
    memcpy(peer.lmk, LMK, sizeof(peer.lmk));
#else
    peer.encrypt = false;
#endif

    if (!esp_now_is_peer_exist(PEER_MAC)) {
        ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    } else {
        ESP_ERROR_CHECK(esp_now_mod_peer(&peer));
    }
    set_peer_rate_config(PEER_MAC);

    /* Очередь (длина 1) и воркер отправки */
    s_txq = xQueueCreate(1, sizeof(tx_item_t));
    configASSERT(s_txq != NULL);
    xTaskCreatePinnedToCore(tx_worker, "espnow_tx", TX_TASK_STACK, NULL,
                            TX_TASK_PRIO, &s_tx_task, 0);

    ESP_LOGI(TAG, "ESP-NOW server ready → %02X:%02X:%02X:%02X:%02X:%02X%s",
             PEER_MAC[0],PEER_MAC[1],PEER_MAC[2],
             PEER_MAC[3],PEER_MAC[4],PEER_MAC[5],
#if ESPNOW_ENCRYPT
             " (encrypted)"
#else
             ""
#endif
    );
    return ESP_OK;
}

int transport_send(const char *msg, size_t len)
{
    if (!msg || len == 0) return 0;
    if (len > ESPNOW_MAX_PAYLOAD) len = ESPNOW_MAX_PAYLOAD;

    tx_item_t it = { .len = (uint16_t)len };
    memcpy(it.data, msg, len);

    // очередь длиной 1 → храним последнее состояние
    (void)xQueueOverwrite(s_txq, &it);
    return (int)len;
}
