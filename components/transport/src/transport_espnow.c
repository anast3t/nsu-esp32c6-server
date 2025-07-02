// transport_espnow_server.c
#include "transport.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "ESPNOW_SRV";
// static const char *AP_SSID = "ESPNOW_AP";
// static const char *AP_PASS = "123123123";        // можно оставить пустым
static const uint8_t ESPNOW_CHANNEL = 5;
static uint8_t broadcast_mac[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

esp_err_t transport_init(void)
{
    // 1) NVS уже инициализирован в app_main
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 2) AP
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid            = "ESPNOW_AP",
            .password        = "",
            .ssid_len        = 0,
            .channel         = ESPNOW_CHANNEL,
            .authmode        = WIFI_AUTH_OPEN,
            .max_connection  = 4,
            .beacon_interval = 100
        }
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

    // ↓↓↓ эти две строки отличают ваш проверенный TCP-сервер от ESPNOW
    ESP_ERROR_CHECK( esp_wifi_set_ps(WIFI_PS_NONE) );
    ESP_ERROR_CHECK( esp_wifi_set_protocol(
        WIFI_IF_AP,
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N
    ) );

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Wi-Fi AP ready (SSID:%s, chan:%d)", "ESPNOW_AP", ESPNOW_CHANNEL);

    // 3) ESPNOW
    ESP_ERROR_CHECK(esp_now_init());
    esp_now_peer_info_t peer = {
        .channel = ESPNOW_CHANNEL,
        .encrypt = false
    };
    memcpy(peer.peer_addr, broadcast_mac, 6);
    peer.ifidx = ESP_IF_WIFI_AP;
    if (!esp_now_is_peer_exist(broadcast_mac)) {
        ESP_ERROR_CHECK( esp_now_add_peer(&peer) );
    } else {
        ESP_ERROR_CHECK( esp_now_mod_peer(&peer) );
    }

    ESP_LOGI(TAG, "ESP-NOW server ready");
    return ESP_OK;
}

int transport_send(const char *msg, size_t len)
{
    esp_err_t res = esp_now_send(broadcast_mac, (uint8_t*)msg, len);
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "esp_now_send err=%s", esp_err_to_name(res));
        return -1;
    }
    return len;
}
