#include "status_leds.h"

#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "status_leds";

static SemaphoreHandle_t s_led_mutex;
static status_leds_mode_t s_mode = STATUS_LEDS_IDLE;

static uint32_t led_level(bool on, int active_level)
{
    const bool active_high = active_level != 0;
    return on == active_high ? 1U : 0U;
}

static void apply_mode(status_leds_mode_t mode)
{
    bool green = false;
    bool red = false;

    switch (mode) {
    case STATUS_LEDS_REGISTERED:
        green = true;
        break;
    case STATUS_LEDS_RINGING:
    case STATUS_LEDS_IN_CALL:
        green = true;
        red = true;
        break;
    case STATUS_LEDS_ERROR:
        red = true;
        break;
    case STATUS_LEDS_IDLE:
    case STATUS_LEDS_REGISTERING:
    default:
        break;
    }

    gpio_set_level(BOARD_LED_GREEN_GPIO, led_level(green, BOARD_LED_GREEN_ACTIVE_LEVEL));
    gpio_set_level(BOARD_LED_RED_GPIO, led_level(red, BOARD_LED_RED_ACTIVE_LEVEL));
}

esp_err_t status_leds_init(void)
{
    s_led_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_led_mutex != NULL, ESP_ERR_NO_MEM, TAG, "create led mutex failed");

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BOARD_LED_GREEN_GPIO) | (1ULL << BOARD_LED_RED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "configure led gpios failed");
    apply_mode(s_mode);

    ESP_LOGI(TAG,
             "status LEDs ready: green GPIO%d active=%d, red GPIO%d active=%d",
             BOARD_LED_GREEN_GPIO,
             BOARD_LED_GREEN_ACTIVE_LEVEL,
             BOARD_LED_RED_GPIO,
             BOARD_LED_RED_ACTIVE_LEVEL);

    return ESP_OK;
}

void status_leds_set_mode(status_leds_mode_t mode)
{
    if (s_led_mutex != NULL && xSemaphoreTake(s_led_mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "take led mutex failed");
        return;
    }

    s_mode = mode;
    apply_mode(mode);

    if (s_led_mutex != NULL) {
        xSemaphoreGive(s_led_mutex);
    }
}
