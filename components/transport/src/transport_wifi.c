#include "transport.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "esp_log.h"

#define MY_AP_SSID   "ESP32C6_AP"
#define MY_AP_PASS   "12345678"
#define TCP_PORT     5000

static int client_fd = -1;
static const char *TAG = "TRANSPORT_WIFI";

typedef enum
{
    WIFI_STD_4,
    WIFI_STD_6
} wifi_std_t;
static esp_err_t wifi_ap_set_proto(wifi_std_t s)
{
    wifi_protocols_t p = {0};
    p.ghz_2g = (s == WIFI_STD_4)
                   ? (WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N)
                   : WIFI_PROTOCOL_11AX;
    return esp_wifi_set_protocols(WIFI_IF_AP, &p);
}

static void tcp_srv_task(void *arg)
{
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in a = {.sin_family = AF_INET,
                            .sin_port = htons(TCP_PORT),
                            .sin_addr.s_addr = htonl(INADDR_ANY)};
    bind(srv, (void *)&a, sizeof(a));
    listen(srv, 1);
    ESP_LOGI(TAG, "TCP server %d ready", TCP_PORT);

    for (;;)
    {
        struct sockaddr_in cli;
        socklen_t len = sizeof(cli);
        int fd = accept(srv, (void *)&cli, &len);
        if (fd >= 0)
        {
            if (client_fd >= 0)
                close(client_fd);
            client_fd = fd;
            ESP_LOGI(TAG, "Client connected");
        }
    }
}

esp_err_t transport_init(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t ap = {.ap = {
                            .ssid = MY_AP_SSID, .password = MY_AP_PASS, .authmode = WIFI_AUTH_WPA2_PSK, .max_connection = 4, .channel = 1}};
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    wifi_ap_set_proto(WIFI_STD_6);
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_start();
    xTaskCreatePinnedToCore(tcp_srv_task, "tcp_srv", 4096, NULL, 8, NULL, 0);

    ESP_LOGI(TAG, "Wi-Fi transport ready");
    return ESP_OK;
}

int transport_send(const char *msg, size_t len)
{
    return (client_fd >= 0) ? send(client_fd, msg, len, MSG_DONTWAIT) : -1;
}