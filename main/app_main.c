#include "audio_hw.h"
#include "board.h"
#include "button.h"
#include "call_recorder.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ipphone_wifi.h"
#include "nvs_flash.h"
#include "sip_phone.h"
#include "status_leds.h"

static const char *TAG = "ipphone";

#define BUTTON_TASK_STACK_BYTES 6144
#define DEFAULT_CALL_TASK_STACK_BYTES 12288

static TaskHandle_t s_default_call_task;

static void apply_sip_state_leds(sip_phone_state_t state)
{
    switch (state) {
    case SIP_PHONE_STATE_REGISTERING:
        status_leds_set_mode(STATUS_LEDS_REGISTERING);
        break;
    case SIP_PHONE_STATE_REGISTERED:
        status_leds_set_mode(STATUS_LEDS_REGISTERED);
        break;
    case SIP_PHONE_STATE_CALLING:
    case SIP_PHONE_STATE_RINGING:
        status_leds_set_mode(STATUS_LEDS_RINGING);
        break;
    case SIP_PHONE_STATE_IN_CALL:
        status_leds_set_mode(STATUS_LEDS_IN_CALL);
        break;
    case SIP_PHONE_STATE_ERROR:
        status_leds_set_mode(STATUS_LEDS_ERROR);
        break;
    case SIP_PHONE_STATE_OFFLINE:
    default:
        status_leds_set_mode(STATUS_LEDS_IDLE);
        break;
    }
}

static void start_network_and_sip(void)
{
    if (!ipphone_wifi_is_configured()) {
        ESP_LOGW(TAG, "Wi-Fi SSID is empty; configure it in menuconfig before SIP registration");
        status_leds_set_mode(STATUS_LEDS_ERROR);
        return;
    }

    status_leds_set_mode(STATUS_LEDS_REGISTERING);
    esp_err_t err = ipphone_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi start failed: %s", esp_err_to_name(err));
        status_leds_set_mode(STATUS_LEDS_ERROR);
        return;
    }

    err = sip_phone_start_registration();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SIP registration failed: %s", esp_err_to_name(err));
        status_leds_set_mode(STATUS_LEDS_ERROR);
        return;
    }

    status_leds_set_mode(STATUS_LEDS_REGISTERED);
}

static void default_call_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "default call task started");
    esp_err_t err = sip_phone_call_default_extension();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "default call failed: %s", esp_err_to_name(err));
        apply_sip_state_leds(sip_phone_get_state());
    } else if (sip_phone_get_state() == SIP_PHONE_STATE_IN_CALL) {
        status_leds_set_mode(STATUS_LEDS_IN_CALL);
    } else {
        apply_sip_state_leds(sip_phone_get_state());
    }

    ESP_LOGI(TAG,
             "default call task finished, stack free=%u words",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    s_default_call_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t start_default_call_task(void)
{
    if (s_default_call_task != NULL) {
        ESP_LOGI(TAG, "default call already in progress");
        return ESP_OK;
    }

    BaseType_t created = xTaskCreate(default_call_task,
                                     "default_call",
                                     DEFAULT_CALL_TASK_STACK_BYTES,
                                     NULL,
                                     5,
                                     &s_default_call_task);
    if (created != pdPASS) {
        s_default_call_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static void button_task(void *arg)
{
    (void)arg;
    sip_phone_state_t last_state = sip_phone_get_state();
    apply_sip_state_leds(last_state);

    while (true) {
        sip_phone_state_t state = sip_phone_get_state();
        if (state != last_state) {
            apply_sip_state_leds(state);
            last_state = state;
        }

        button_event_t event = button_read_event(CONFIG_IPPHONE_BUTTON_HOLD_MS);

        if (event == BUTTON_EVENT_CLICK) {
            state = sip_phone_get_state();
            if (state == SIP_PHONE_STATE_RINGING) {
                ESP_LOGI(TAG, "button click: answer incoming SIP call");
                esp_err_t err = sip_phone_answer();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "answer failed: %s", esp_err_to_name(err));
                    status_leds_set_mode(STATUS_LEDS_ERROR);
                } else {
                    status_leds_set_mode(STATUS_LEDS_IN_CALL);
                }
                continue;
            }

            if (state == SIP_PHONE_STATE_CALLING || state == SIP_PHONE_STATE_IN_CALL) {
                ESP_LOGI(TAG, "button click: hang up SIP call");
                esp_err_t err = sip_phone_hangup();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "hangup failed: %s", esp_err_to_name(err));
                    status_leds_set_mode(STATUS_LEDS_ERROR);
                } else {
                    apply_sip_state_leds(sip_phone_get_state());
                }
                continue;
            }

            ESP_LOGI(TAG, "button click ignored in state %d", state);
        } else if (event == BUTTON_EVENT_HOLD) {
            state = sip_phone_get_state();
            if (state == SIP_PHONE_STATE_REGISTERED) {
                ESP_LOGI(TAG, "button hold: call default SIP extension");
                esp_err_t err = start_default_call_task();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "start default call task failed: %s", esp_err_to_name(err));
                    apply_sip_state_leds(sip_phone_get_state());
                }
            } else if (state == SIP_PHONE_STATE_CALLING || state == SIP_PHONE_STATE_IN_CALL || state == SIP_PHONE_STATE_RINGING) {
                ESP_LOGI(TAG, "button hold: hang up SIP call");
                esp_err_t err = sip_phone_hangup();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "hangup failed: %s", esp_err_to_name(err));
                    status_leds_set_mode(STATUS_LEDS_ERROR);
                } else {
                    apply_sip_state_leds(sip_phone_get_state());
                }
            } else {
                ESP_LOGI(TAG, "button hold ignored in state %d", state);
            }
            button_wait_for_release();
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(board_log_pinout());
    ESP_ERROR_CHECK(status_leds_init());
    ESP_ERROR_CHECK(button_init());
    ESP_ERROR_CHECK(audio_hw_init());
    ESP_ERROR_CHECK(sip_phone_init());
    err = call_recorder_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "call recorder disabled: %s", esp_err_to_name(err));
    }

    ESP_ERROR_CHECK(audio_hw_play_test_tone());
    start_network_and_sip();

    xTaskCreate(button_task, "button_task", BUTTON_TASK_STACK_BYTES, NULL, 5, NULL);
}
