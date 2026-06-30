#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    CALL_RECORDER_AUDIO_RX,
    CALL_RECORDER_AUDIO_TX,
} call_recorder_audio_dir_t;

esp_err_t call_recorder_init(void);
bool call_recorder_is_ready(void);
esp_err_t call_recorder_start(const char *caller, const char *callee);
void call_recorder_stop(void);
void call_recorder_push_audio(call_recorder_audio_dir_t dir, const int16_t *samples, size_t sample_count);
