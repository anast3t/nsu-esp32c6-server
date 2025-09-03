#include "led_ctrl.h"
#include "driver/gpio.h"
#include "led_strip.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "transport.h"      // transport_send()
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"      // esp_timer_*()

#define LED_GPIO    8
#define LED_COUNT   1
#define BOOT_BTN    9
#define PULSE_GPIO  10       // Пин для 5мс импульса при «мигании» LED
#define PULSE_US    1000     // Длительность импульса, мкс

static led_strip_handle_t strip;
static TaskHandle_t       led_task;
static volatile uint32_t  t_cycle;
static const char        *TAG = "LED_CTRL";

// ---- ПУЛЬС: неблокирующий одноразовый таймер ----
static esp_timer_handle_t s_pulse_timer;

static void pulse_off_cb(void *arg) {
    // Колбэк в контексте задачи таймера (не ISR)
    gpio_set_level(PULSE_GPIO, 0);
}

static inline void pulse_start_or_restart_us(uint32_t us) {
    // Поднять пин, «перезапустить» одноразовый таймер
    gpio_set_level(PULSE_GPIO, 1);
    (void)esp_timer_stop(s_pulse_timer);           // ок, если уже остановлен
    (void)esp_timer_start_once(s_pulse_timer, us); // опустим через us
}
// --------------------------------------------------

// Таблица цветов: R,G,B (0–32)
static const uint8_t color_table[][3] = {
    {32,  0,  0},   // красный
    { 0, 32,  0},   // зелёный
    { 0,  0, 32},   // синий
    {32, 32,  0},   // жёлтый
    { 0, 32, 32},   // циан
    {32,  0, 32},   // пурпур
};
static const size_t COLOR_COUNT = sizeof(color_table) / sizeof(color_table[0]);

static void IRAM_ATTR btn_isr(void *arg) {
    t_cycle = esp_cpu_get_cycle_count();
    BaseType_t hp = pdFALSE;
    vTaskNotifyGiveFromISR(led_task, &hp);
    if (hp) portYIELD_FROM_ISR();
}

static void led_task_fn(void *arg) {
    size_t idx = 0;
    const uint32_t cpu_hz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000UL;

    for (;;) {
        // ждём нотификацию или от кнопки, или от периодического таска
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // выбираем цвет и рисуем
        // uint8_t r = color_table[idx][0];
        // uint8_t g = color_table[idx][1];
        // uint8_t b = color_table[idx][2];
        // led_strip_set_pixel(strip, 0, r, g, b);
        // led_strip_refresh(strip);

        // --- 5мс импульс НЕБЛОКИРУЮЩЕ ---
        pulse_start_or_restart_us(PULSE_US);
        // --------------------------------

        // отправляем строку "COL:R,G,B\n"
        char buf[32];
        // int n = snprintf(buf, sizeof(buf), "COL:%u,%u,%u\n", r, g, b);
        int n = snprintf(buf, sizeof(buf), "MSG");
        transport_send(buf, n);

        // лог времени от прерывания до обработки
        // // uint32_t d   = esp_cpu_get_cycle_count() - t_cycle;
        // float    ns  = (float)d * 1e9f / cpu_hz;
        // ESP_LOGI(TAG,
        //          "COLOR %u → R%u,G%u,B%u | %.3f µs | %.3f ms",
        //          idx, r, g, b, ns/1e3f, ns/1e6f);

        // следующий цвет по кругу
        // idx = (idx + 1) % COLOR_COUNT;
    }
}

// Периодический таск — подтолкнёт led_task_fn() каждые ~400 ms
static void periodic_task(void *arg)
{
    for (;;) {
        t_cycle = esp_cpu_get_cycle_count();
        xTaskNotifyGive(led_task);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

esp_err_t led_ctrl_init(void) {
    // инициализация ленты
    // led_strip_config_t cfg = {
    //     .strip_gpio_num = LED_GPIO,
    //     .max_leds       = LED_COUNT,
    //     .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    //     .led_model      = LED_MODEL_WS2812
    // };
    // led_strip_rmt_config_t rmt = {
    //     .clk_src       = RMT_CLK_SRC_DEFAULT,
    //     .resolution_hz = 10 * 1000 * 1000
    // };
    // ESP_ERROR_CHECK(led_strip_new_rmt_device(&cfg, &rmt, &strip));

    // кнопка + ISR
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_BTN,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_IRAM));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BOOT_BTN, btn_isr, NULL));

    // пин импульса (низкий по умолчанию)
    gpio_config_t pulse = {
        .pin_bit_mask = 1ULL << PULSE_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&pulse));
    gpio_set_level(PULSE_GPIO, 0);

    // одноразовый таймер для импульса
    const esp_timer_create_args_t targs = {
        .callback        = &pulse_off_cb,
        .name            = "pulse5ms",
        .dispatch_method = ESP_TIMER_TASK,   // колбэк не в ISR, ограничений меньше
        .arg             = NULL
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_pulse_timer));

    // создаём таски
    xTaskCreatePinnedToCore(led_task_fn,   "led_task",  2048, NULL, 10, &led_task,    0);
    xTaskCreatePinnedToCore(periodic_task, "periodic",  2048, NULL,  9, NULL,         0);

    ESP_LOGI(TAG, "LED controller ready: %u colors", COLOR_COUNT);
    return ESP_OK;
}
