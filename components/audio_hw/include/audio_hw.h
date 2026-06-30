#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t audio_hw_init(void);
esp_err_t audio_hw_play_test_tone(void);
esp_err_t audio_hw_record_and_playback_test(uint32_t duration_ms);
esp_err_t audio_hw_mic_start(void);
esp_err_t audio_hw_mic_read(int16_t *samples, size_t sample_capacity, size_t *samples_read, int32_t *peak);
esp_err_t audio_hw_mic_stop(void);
esp_err_t audio_hw_speaker_start(void);
esp_err_t audio_hw_speaker_write(const int16_t *samples, size_t sample_count);
esp_err_t audio_hw_speaker_stop(void);
