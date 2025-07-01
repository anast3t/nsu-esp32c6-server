#include "nvs_flash.h"
#include "esp_log.h"
#include "app_main.h"
#include "transport.h"
#include "led_ctrl.h"

void app_main(void) {
    esp_log_level_set("*", ESP_LOG_NONE);

    ESP_ERROR_CHECK(nvs_flash_init());
    // esp_log_level_set("NimBLE", ESP_LOG_NONE);

    // Инициализация транспорта (Wi-Fi или BLE)
    ESP_ERROR_CHECK(transport_init());

    // Инициализация работы с LED и кнопкой
    ESP_ERROR_CHECK(led_ctrl_init());
}
