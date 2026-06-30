#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef enum {
    STATUS_LEDS_IDLE,
    STATUS_LEDS_REGISTERING,
    STATUS_LEDS_REGISTERED,
    STATUS_LEDS_RINGING,
    STATUS_LEDS_IN_CALL,
    STATUS_LEDS_ERROR,
} status_leds_mode_t;

esp_err_t status_leds_init(void);
void status_leds_set_mode(status_leds_mode_t mode);
