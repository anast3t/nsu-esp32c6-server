#include "led_ctrl.h"
#include "driver/gpio.h"
#include "led_strip.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "transport.h"      // transport_send()
#include "esp_err.h"

#define LED_GPIO 8
#define LED_COUNT 1
#define BOOT_BTN 9

static led_strip_handle_t strip;
static TaskHandle_t       led_task;
static volatile uint32_t  t_cycle;
static bool               led_on = false;
static const char        *TAG = "LED_CTRL";

static void IRAM_ATTR btn_isr(void *arg) {
    t_cycle = esp_cpu_get_cycle_count();
    BaseType_t hp = pdFALSE;
    vTaskNotifyGiveFromISR(led_task, &hp);
    if (hp) portYIELD_FROM_ISR();
}

static void led_task_fn(void *arg) {
    bool on = false;
    const uint32_t cpu = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000UL;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        led_strip_set_pixel(strip, 0, on ? 32 : 0, 0, 0);
        led_strip_refresh(strip);
        led_on = on;
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "LED:%s\n", on ? "ON" : "OFF");
        transport_send(buf, n);
        uint32_t d = esp_cpu_get_cycle_count() - t_cycle;
        float ns = d * 1e9f / cpu;
        ESP_LOGI(TAG, "LED %s | %.3f µs | %.3f ms", on?"ON":"OFF", ns/1e3, ns/1e6);
        on = !on;
    }
}

// сюда же вверху файла, после объявления led_task и t_cycle
static void periodic_task(void *arg)
{
    for (;;) {
        // засечём момент «имитации нажатия»
        t_cycle = esp_cpu_get_cycle_count();
        // уведомим led_task ровно так же, как ISR
        xTaskNotifyGive(led_task);
        // ждём полсекунды
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}


esp_err_t led_ctrl_init(void) {
    // инициализация ленты
    led_strip_config_t cfg = { .strip_gpio_num = LED_GPIO, .max_leds = LED_COUNT,
                               .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
                               .led_model = LED_MODEL_WS2812 };
    led_strip_rmt_config_t rmt = { .clk_src = RMT_CLK_SRC_DEFAULT,
                                    .resolution_hz = 10 * 1000 * 1000 };
    led_strip_new_rmt_device(&cfg, &rmt, &strip);

    // кнопка + ISR
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_BTN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = 1,
        .intr_type = GPIO_INTR_NEGEDGE
    };
    gpio_config(&io);
    gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    gpio_isr_handler_add(BOOT_BTN, btn_isr, NULL);

    xTaskCreatePinnedToCore(led_task_fn, "led_task", 2048, NULL, 10, &led_task, 0);
    xTaskCreatePinnedToCore(
        periodic_task, "periodic", 2048, NULL, 9, NULL, 0
    );
    ESP_LOGI(TAG, "LED controller ready");
    return ESP_OK;
}

bool led_ctrl_is_on(void) {
    return led_on;
}

// #include "led_ctrl.h"
// #include "driver/gpio.h"
// #include "esp_cpu.h"
// #include "esp_log.h"
// #include "transport.h"      // transport_send()
// #include "esp_err.h"
// #include "freertos/FreeRTOS.h"
// #include "freertos/task.h"

// #define LED_GPIO   8
// #define BOOT_BTN   9
// static TaskHandle_t led_task;
// static volatile uint32_t t_cycle;
// static bool led_on = false;
// static const char *TAG = "LED_CTRL";

// /** ISR: на падение BOOT_BTN сохраняем такты и нотифицируем таск */
// static void IRAM_ATTR btn_isr(void *arg) {
//     t_cycle = esp_cpu_get_cycle_count();
//     BaseType_t hp = pdFALSE;
//     vTaskNotifyGiveFromISR(led_task, &hp);
//     if (hp) portYIELD_FROM_ISR();
// }

// /** Таск: ждём нотификацию, переключаем GPIO8, шлём текст и логируем */
// static void led_task_fn(void *arg) {
//     bool on = false;
//     const uint32_t cpu_hz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000UL;
//     for (;;) {
//         ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

//         // Переключаем GPIO8
//         gpio_set_level(LED_GPIO, on ? 1 : 0);
//         led_on = on;

//         // Отправляем по транспорту
//         char buf[32];
//         int n = snprintf(buf, sizeof(buf), "LED:%s\n", on ? "ON" : "OFF");
//         transport_send(buf, n);

//         // Засекаем время от ISR до обработки
//         uint32_t d = esp_cpu_get_cycle_count() - t_cycle;
//         float ns = (float)d * 1e9f / cpu_hz;
//         ESP_LOGI(TAG, "LED %s | %.3f µs | %.3f ms",
//                  on ? "ON" : "OFF", ns / 1e3f, ns / 1e6f);

//         on = !on;
//     }
// }

// esp_err_t led_ctrl_init(void) {
//     // Настраиваем LED_GPIO как выход, сбрасываем в 0
//     gpio_config_t io_led = {
//         .pin_bit_mask = 1ULL << LED_GPIO,
//         .mode         = GPIO_MODE_OUTPUT,
//         .pull_up_en   = GPIO_PULLUP_DISABLE,
//         .pull_down_en = GPIO_PULLDOWN_DISABLE,
//         .intr_type    = GPIO_INTR_DISABLE
//     };
//     ESP_ERROR_CHECK(gpio_config(&io_led));
//     gpio_set_level(LED_GPIO, 0);

//     // Настраиваем BOOT_BTN как вход с прерыванием по спаду
//     gpio_config_t io_btn = {
//         .pin_bit_mask = 1ULL << BOOT_BTN,
//         .mode         = GPIO_MODE_INPUT,
//         .pull_up_en   = GPIO_PULLUP_ENABLE,
//         .pull_down_en = GPIO_PULLDOWN_DISABLE,
//         .intr_type    = GPIO_INTR_NEGEDGE
//     };
//     ESP_ERROR_CHECK(gpio_config(&io_btn));
//     gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
//     gpio_isr_handler_add(BOOT_BTN, btn_isr, NULL);

//     // Создаём таск для обработки нажатий
//     xTaskCreatePinnedToCore(led_task_fn,
//                             "led_task",
//                             2048,
//                             NULL,
//                             10,
//                             &led_task,
//                             0);

//     ESP_LOGI(TAG, "LED controller ready (GPIO %d)", LED_GPIO);
//     return ESP_OK;
// }

// bool led_ctrl_is_on(void) {
//     return led_on;
// }
