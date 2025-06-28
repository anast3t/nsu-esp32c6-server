#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_mac.h"    
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "led_strip.h"
#include "lwip/sockets.h"
#include "esp_cpu.h"    



#define BOOT_BUTTON_GPIO  9
#define LED_STRIP_GPIO    8
#define LED_STRIP_COUNT   1


#define MY_AP_SSID        "ESP32C6_AP"
#define MY_AP_PASS        "12345678"
#define TCP_PORT          5000

static const char *TAG = "AP_LED_TCP";


static led_strip_handle_t led_strip;
static TaskHandle_t       led_task_handle;
static TaskHandle_t       tcp_task_handle;
static volatile uint32_t  cycle_start = 0;
static int                client_fd   = -1;   


static void IRAM_ATTR button_isr(void *arg)
{
    cycle_start = esp_cpu_get_cycle_count();
    BaseType_t hp = pdFALSE;
    vTaskNotifyGiveFromISR(led_task_handle, &hp);
    if (hp) portYIELD_FROM_ISR();
}

static void tcp_send_log(bool led_on, uint32_t cycles)
{
    if (client_fd < 0) return;

    float ns   = (float)cycles * 1e9f / 160000000.0;
    float ms   = ns / 1e6f;

    char buf[96];
    int len = snprintf(buf, sizeof(buf),
                       "LED:%s cycles:%lu time:%.2fns (%.6fms)\n",
                       led_on ? "ON" : "OFF", cycles, ns, ms);

    if (send(client_fd, buf, len, 0) < 0) {
        ESP_LOGW(TAG, "send() error, closing socket");
        close(client_fd);
        client_fd = -1;
    }
}


static void tcp_server_task(void *arg)
{
    int srv_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port        = htons(TCP_PORT)
    };
    bind(srv_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(srv_fd, 1);
    ESP_LOGI(TAG, "TCP server listening on %d", TCP_PORT);

    for (;;) {
        struct sockaddr_in cli;
        socklen_t          len = sizeof(cli);
        int fd = accept(srv_fd, (struct sockaddr *)&cli, &len);
        if (fd >= 0) {
            if (client_fd >= 0) close(client_fd); 
            client_fd = fd;
            ESP_LOGI(TAG, "Client connected");
        }
    }
}


static void led_task(void *arg)
{
    bool led_on = false;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        uint8_t val = led_on ? 32 : 0;
        led_strip_set_pixel(led_strip, 0, val, 0, 0);
        led_strip_refresh(led_strip);
        led_on = !led_on;
        uint32_t cycles = esp_cpu_get_cycle_count() - cycle_start;
        float ns = (float)cycles * 1e9f / (float)160000000;
        float ms = ns / 1e6f;
        ESP_LOGI(TAG, "LED %s | cycles %lu | %.2f ns (%.6f ms)",
                 led_on ? "ON" : "OFF", cycles, ns, ms);

        tcp_send_log(led_on, cycles);
    }
}


/**
 * Поддерживаемые «жёсткие» профили для C6
 *  – Wi‑Fi 4 (11b/g/n)
 *  – Wi‑Fi 6 (11ax)
 * Варианта «Wi‑Fi 5» нет, поскольку чип не работает в 5 ГГц.
 */
typedef enum {
    WIFI_STD_4,   // 802.11b/g/n
    WIFI_STD_6    // 802.11ax
} wifi_standard_t;

/**
 * @brief  Привязать SoftAP к единственному протоколу (BGN‑only или AX‑only)
 */
static esp_err_t wifi_ap_set_standard_strict(wifi_standard_t std)
{
    wifi_protocols_t proto = {0};

    switch (std) {
        case WIFI_STD_4:
            /* B | G | N – классический Wi‑Fi 4 */
            proto.ghz_2g = WIFI_PROTOCOL_11B |
                           WIFI_PROTOCOL_11G |
                           WIFI_PROTOCOL_11N;
            break;
        case WIFI_STD_6:
            /* Чистый 802.11ax @2.4 ГГц */
            proto.ghz_2g = WIFI_PROTOCOL_11AX;
            break;
        default:
            return ESP_ERR_INVALID_ARG;
    }

    return esp_wifi_set_protocols(WIFI_IF_AP, &proto);
}

void wifi_init_softap(void)
{
    /* 1. Сетевой стек и события */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    /* 2. Инициализация Wi‑Fi драйвера */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* 3. Параметры точки доступа */
    wifi_config_t ap_cfg = {
        .ap = {
            .ssid           = MY_AP_SSID,
            .password       = MY_AP_PASS,
            .ssid_len       = 0,
            .authmode       = WIFI_AUTH_WPA2_PSK,
            .max_connection = 4,
            .channel        = 1
        }
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

    /* 4. Жёстко задаём протокол: Wi‑Fi 6 only  */
    ESP_ERROR_CHECK(wifi_ap_set_standard_strict(WIFI_STD_6));

    /* 5. Старт SoftAP */
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "SoftAP started: %s (Wi‑Fi 6 only)", MY_AP_SSID);
}


void app_main(void)
{
    nvs_flash_init();
    wifi_init_softap();

    led_strip_config_t cfg = {
        .strip_gpio_num         = LED_STRIP_GPIO,
        .max_leds               = LED_STRIP_COUNT,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .led_model              = LED_MODEL_WS2812,
    };
    led_strip_rmt_config_t rmt = {
        .clk_src       = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000
    };
    led_strip_new_rmt_device(&cfg, &rmt, &led_strip);
    led_strip_set_pixel(led_strip, 0, 0, 0, 0);
    led_strip_refresh(led_strip);

    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = 1,
        .intr_type    = GPIO_INTR_NEGEDGE
    };
    gpio_config(&btn);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BOOT_BUTTON_GPIO, button_isr, NULL);

    xTaskCreatePinnedToCore(led_task,        "led_task",  3072, NULL, 10, &led_task_handle, 0);
    xTaskCreatePinnedToCore(tcp_server_task, "tcp_srv",   4096, NULL,  9, &tcp_task_handle, 0);

    ESP_LOGI(TAG, "Ready: connect to AP '%s' and TCP %s:%d",
             MY_AP_SSID, "192.168.4.1", TCP_PORT);
}
