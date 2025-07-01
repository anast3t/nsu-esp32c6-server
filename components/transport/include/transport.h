#pragma once

#include "led_ctrl.h"
#include "esp_err.h"
#include <stddef.h>
esp_err_t transport_init(void);
int       transport_send(const char *msg, size_t len);
