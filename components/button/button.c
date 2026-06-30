#include "button.h"

#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "button";

static bool button_pressed_raw(void)
{
    return gpio_get_level(BOARD_BUTTON_GPIO) == BOARD_BUTTON_ACTIVE_LEVEL;
}

esp_err_t button_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOARD_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = BOARD_BUTTON_ACTIVE_LEVEL == 0 ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = BOARD_BUTTON_ACTIVE_LEVEL == 0 ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "configure button gpio failed");
    ESP_LOGI(TAG,
             "button ready: GPIO%d active=%d debounce=%d ms",
             BOARD_BUTTON_GPIO,
             BOARD_BUTTON_ACTIVE_LEVEL,
             CONFIG_IPPHONE_BUTTON_DEBOUNCE_MS);
    return ESP_OK;
}

bool button_is_pressed(void)
{
    return button_pressed_raw();
}

bool button_release_confirmed(void)
{
    if (button_pressed_raw()) {
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(CONFIG_IPPHONE_BUTTON_DEBOUNCE_MS));
    return !button_pressed_raw();
}

button_event_t button_read_event(uint32_t hold_ms)
{
    if (!button_pressed_raw()) {
        return BUTTON_EVENT_NONE;
    }

    vTaskDelay(pdMS_TO_TICKS(CONFIG_IPPHONE_BUTTON_DEBOUNCE_MS));
    if (!button_pressed_raw()) {
        return BUTTON_EVENT_NONE;
    }

    const TickType_t press_start = xTaskGetTickCount();
    const TickType_t hold_ticks = pdMS_TO_TICKS(hold_ms);

    while (button_pressed_raw()) {
        if ((xTaskGetTickCount() - press_start) >= hold_ticks) {
            return BUTTON_EVENT_HOLD;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    return BUTTON_EVENT_CLICK;
}

void button_wait_for_release(void)
{
    while (button_pressed_raw()) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    vTaskDelay(pdMS_TO_TICKS(CONFIG_IPPHONE_BUTTON_DEBOUNCE_MS));
}
