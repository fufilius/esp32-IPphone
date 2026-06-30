#include "audio_hw.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "board_pins.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "audio_hw";

static i2s_chan_handle_t s_mic_rx_chan;
static i2s_chan_handle_t s_spk_tx_chan;
static SemaphoreHandle_t s_mic_mutex;
static SemaphoreHandle_t s_speaker_mutex;
static TaskHandle_t s_mic_owner;
static TaskHandle_t s_speaker_owner;
static bool s_mic_busy;
static bool s_speaker_busy;

#define MIC_READ_SAMPLES 256
#define TEST_TONE_HZ 880
#define TEST_TONE_MS 350
#define TEST_TONE_AMPLITUDE 9000
#define AUDIO_PI 3.14159265358979323846f
#define RECORD_PLAYBACK_MS 1200
#define MIC_TO_SPEAKER_SHIFT 13

static int16_t mic_sample_to_pcm16(int32_t mic_sample)
{
    int32_t sample = mic_sample >> MIC_TO_SPEAKER_SHIFT;
    if (sample > INT16_MAX) {
        sample = INT16_MAX;
    } else if (sample < INT16_MIN) {
        sample = INT16_MIN;
    }
    return (int16_t)sample;
}

static bool current_task_owns_mic(void)
{
    return s_mic_owner == xTaskGetCurrentTaskHandle();
}

static bool current_task_owns_speaker(void)
{
    return s_speaker_owner == xTaskGetCurrentTaskHandle();
}

esp_err_t audio_hw_init(void)
{
    s_mic_mutex = xSemaphoreCreateMutex();
    s_speaker_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_mic_mutex != NULL && s_speaker_mutex != NULL,
                        ESP_ERR_NO_MEM,
                        TAG,
                        "create audio mutexes failed");

    ESP_LOGI(TAG,
             "I2S mic: BCLK=%d WS=%d DIN=%d; speaker: BCLK=%d WS=%d DOUT=%d; sample_rate=%d",
             BOARD_I2S_MIC_BCLK,
             BOARD_I2S_MIC_WS,
             BOARD_I2S_MIC_DIN,
             BOARD_I2S_SPK_BCLK,
             BOARD_I2S_SPK_WS,
             BOARD_I2S_SPK_DOUT,
             CONFIG_IPPHONE_I2S_SAMPLE_RATE);

    i2s_chan_config_t mic_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&mic_chan_cfg, NULL, &s_mic_rx_chan), TAG, "create mic rx channel failed");

    i2s_std_config_t mic_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(CONFIG_IPPHONE_I2S_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BOARD_I2S_MIC_BCLK,
            .ws = BOARD_I2S_MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = BOARD_I2S_MIC_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    mic_std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_mic_rx_chan, &mic_std_cfg), TAG, "init mic rx failed");

    i2s_chan_config_t spk_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&spk_chan_cfg, &s_spk_tx_chan, NULL), TAG, "create speaker tx channel failed");

    i2s_std_config_t spk_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(CONFIG_IPPHONE_I2S_SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BOARD_I2S_SPK_BCLK,
            .ws = BOARD_I2S_SPK_WS,
            .dout = BOARD_I2S_SPK_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    spk_std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_spk_tx_chan, &spk_std_cfg), TAG, "init speaker tx failed");

    ESP_LOGI(TAG, "audio hardware ready");
    return ESP_OK;
}

esp_err_t audio_hw_speaker_start(void)
{
    ESP_RETURN_ON_FALSE(s_spk_tx_chan != NULL, ESP_ERR_INVALID_STATE, TAG, "speaker tx channel is not initialized");
    ESP_RETURN_ON_FALSE(s_speaker_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "speaker mutex is not initialized");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_speaker_mutex, 0) == pdTRUE,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "speaker is busy");

    esp_err_t err = i2s_channel_enable(s_spk_tx_chan);
    if (err != ESP_OK) {
        xSemaphoreGive(s_speaker_mutex);
        ESP_LOGE(TAG, "enable speaker tx failed: %s", esp_err_to_name(err));
        return err;
    }

    s_speaker_busy = true;
    s_speaker_owner = xTaskGetCurrentTaskHandle();
    return ESP_OK;
}

esp_err_t audio_hw_speaker_write(const int16_t *samples, size_t sample_count)
{
    ESP_RETURN_ON_FALSE(samples != NULL, ESP_ERR_INVALID_ARG, TAG, "speaker samples buffer is null");
    ESP_RETURN_ON_FALSE(s_speaker_busy, ESP_ERR_INVALID_STATE, TAG, "speaker is not started");
    ESP_RETURN_ON_FALSE(current_task_owns_speaker(), ESP_ERR_INVALID_STATE, TAG, "speaker is owned by another task");

    const uint8_t *data = (const uint8_t *)samples;
    size_t bytes_remaining = sample_count * sizeof(samples[0]);

    while (bytes_remaining > 0) {
        size_t bytes_written = 0;
        ESP_RETURN_ON_ERROR(
            i2s_channel_write(s_spk_tx_chan, data, bytes_remaining, &bytes_written, pdMS_TO_TICKS(1000)),
            TAG,
            "speaker write failed");
        ESP_RETURN_ON_FALSE(bytes_written > 0, ESP_ERR_TIMEOUT, TAG, "speaker write made no progress");
        data += bytes_written;
        bytes_remaining -= bytes_written;
    }

    return ESP_OK;
}

esp_err_t audio_hw_speaker_stop(void)
{
    ESP_RETURN_ON_FALSE(s_spk_tx_chan != NULL, ESP_ERR_INVALID_STATE, TAG, "speaker tx channel is not initialized");
    ESP_RETURN_ON_FALSE(s_speaker_busy, ESP_ERR_INVALID_STATE, TAG, "speaker is not started");
    ESP_RETURN_ON_FALSE(current_task_owns_speaker(), ESP_ERR_INVALID_STATE, TAG, "speaker is owned by another task");

    esp_err_t err = i2s_channel_disable(s_spk_tx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "disable speaker tx failed: %s", esp_err_to_name(err));
    }

    s_speaker_busy = false;
    s_speaker_owner = NULL;
    xSemaphoreGive(s_speaker_mutex);
    return err;
}

esp_err_t audio_hw_mic_start(void)
{
    ESP_RETURN_ON_FALSE(s_mic_rx_chan != NULL, ESP_ERR_INVALID_STATE, TAG, "mic rx channel is not initialized");
    ESP_RETURN_ON_FALSE(s_mic_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "mic mutex is not initialized");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_mic_mutex, 0) == pdTRUE, ESP_ERR_INVALID_STATE, TAG, "mic is busy");

    s_mic_owner = xTaskGetCurrentTaskHandle();
    esp_err_t err = i2s_channel_enable(s_mic_rx_chan);
    if (err != ESP_OK) {
        s_mic_owner = NULL;
        xSemaphoreGive(s_mic_mutex);
        ESP_LOGE(TAG, "enable mic rx failed: %s", esp_err_to_name(err));
        return err;
    }

    s_mic_busy = true;
    return ESP_OK;
}

esp_err_t audio_hw_mic_read(int16_t *samples, size_t sample_capacity, size_t *samples_read, int32_t *peak)
{
    ESP_RETURN_ON_FALSE(samples != NULL, ESP_ERR_INVALID_ARG, TAG, "mic samples buffer is null");
    ESP_RETURN_ON_FALSE(sample_capacity > 0, ESP_ERR_INVALID_ARG, TAG, "mic samples capacity is zero");
    ESP_RETURN_ON_FALSE(s_mic_busy, ESP_ERR_INVALID_STATE, TAG, "mic is not started");
    ESP_RETURN_ON_FALSE(current_task_owns_mic(), ESP_ERR_INVALID_STATE, TAG, "mic is owned by another task");

    int32_t mic_chunk[MIC_READ_SAMPLES] = {0};
    const size_t samples_to_read = sample_capacity < MIC_READ_SAMPLES ? sample_capacity : MIC_READ_SAMPLES;

    size_t bytes_read = 0;
    esp_err_t err =
        i2s_channel_read(s_mic_rx_chan, mic_chunk, samples_to_read * sizeof(mic_chunk[0]), &bytes_read, pdMS_TO_TICKS(1000));
    ESP_RETURN_ON_ERROR(err, TAG, "mic read failed");

    const size_t read_count = bytes_read / sizeof(mic_chunk[0]);
    ESP_RETURN_ON_FALSE(read_count > 0, ESP_ERR_TIMEOUT, TAG, "mic read made no progress");

    int32_t local_peak = 0;
    for (size_t i = 0; i < read_count; ++i) {
        const int16_t sample = mic_sample_to_pcm16(mic_chunk[i]);
        const int32_t abs_sample = abs(sample);
        if (abs_sample > local_peak) {
            local_peak = abs_sample;
        }
        samples[i] = sample;
    }

    if (samples_read != NULL) {
        *samples_read = read_count;
    }
    if (peak != NULL) {
        *peak = local_peak;
    }
    return ESP_OK;
}

esp_err_t audio_hw_mic_stop(void)
{
    ESP_RETURN_ON_FALSE(s_mic_rx_chan != NULL, ESP_ERR_INVALID_STATE, TAG, "mic rx channel is not initialized");
    ESP_RETURN_ON_FALSE(s_mic_busy, ESP_ERR_INVALID_STATE, TAG, "mic is not started");
    ESP_RETURN_ON_FALSE(current_task_owns_mic(), ESP_ERR_INVALID_STATE, TAG, "mic is owned by another task");

    esp_err_t err = i2s_channel_disable(s_mic_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "disable mic rx failed: %s", esp_err_to_name(err));
    }

    s_mic_busy = false;
    s_mic_owner = NULL;
    xSemaphoreGive(s_mic_mutex);
    return err;
}

esp_err_t audio_hw_play_test_tone(void)
{
    ESP_RETURN_ON_ERROR(audio_hw_speaker_start(), TAG, "start speaker failed");

    const int sample_rate = CONFIG_IPPHONE_I2S_SAMPLE_RATE;
    const int total_samples = (sample_rate * TEST_TONE_MS) / 1000;
    int16_t samples[128] = {0};
    int samples_written = 0;
    esp_err_t err = ESP_OK;

    while (samples_written < total_samples) {
        const int max_batch = (int)(sizeof(samples) / sizeof(samples[0]));
        const int remaining = total_samples - samples_written;
        const int batch = remaining < max_batch ? remaining : max_batch;

        for (int i = 0; i < batch; ++i) {
            const float phase =
                2.0f * AUDIO_PI * (float)TEST_TONE_HZ * (float)(samples_written + i) / (float)sample_rate;
            samples[i] = (int16_t)(sinf(phase) * TEST_TONE_AMPLITUDE);
        }

        err = audio_hw_speaker_write(samples, batch);
        if (err != ESP_OK) {
            break;
        }
        samples_written += batch;
    }

    esp_err_t stop_err = audio_hw_speaker_stop();
    return err == ESP_OK ? stop_err : err;
}

esp_err_t audio_hw_record_and_playback_test(uint32_t duration_ms)
{
    if (duration_ms == 0) {
        duration_ms = RECORD_PLAYBACK_MS;
    }

    const size_t sample_count = ((size_t)CONFIG_IPPHONE_I2S_SAMPLE_RATE * duration_ms) / 1000;
    int16_t *recorded = (int16_t *)calloc(sample_count, sizeof(recorded[0]));
    ESP_RETURN_ON_FALSE(recorded != NULL, ESP_ERR_NO_MEM, TAG, "record buffer allocation failed");

    ESP_LOGI(TAG, "recording voice for %lu ms", (unsigned long)duration_ms);

    int32_t peak = 0;
    esp_err_t err = audio_hw_mic_start();
    if (err == ESP_OK) {
        size_t recorded_count = 0;
        while (recorded_count < sample_count) {
            size_t just_read = 0;
            int32_t chunk_peak = 0;
            err = audio_hw_mic_read(&recorded[recorded_count], sample_count - recorded_count, &just_read, &chunk_peak);
            if (err != ESP_OK || just_read == 0) {
                break;
            }
            recorded_count += just_read;
            if (chunk_peak > peak) {
                peak = chunk_peak;
            }
        }

        esp_err_t stop_err = audio_hw_mic_stop();
        if (err == ESP_OK) {
            err = stop_err;
        }
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "record complete, peak=%ld; playing back", peak);
        err = audio_hw_speaker_start();
        if (err == ESP_OK) {
            err = audio_hw_speaker_write(recorded, sample_count);
            esp_err_t stop_err = audio_hw_speaker_stop();
            if (err == ESP_OK) {
                err = stop_err;
            }
        }
    }

    free(recorded);
    return err;
}
