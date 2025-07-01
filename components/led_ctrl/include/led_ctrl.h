#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

esp_err_t led_ctrl_init(void);


// Новая функция: возвращает текущее состояние LED
bool led_ctrl_is_on(void);