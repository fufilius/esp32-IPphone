#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    BUTTON_EVENT_NONE,
    BUTTON_EVENT_CLICK,
    BUTTON_EVENT_HOLD,
} button_event_t;

esp_err_t button_init(void);
bool button_is_pressed(void);
bool button_release_confirmed(void);
button_event_t button_read_event(uint32_t hold_ms);
void button_wait_for_release(void);
