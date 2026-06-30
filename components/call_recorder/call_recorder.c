#include "call_recorder.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "board_pins.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "esp_private/sdmmc_common.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#include "sdkconfig.h"

static const char *TAG = "call_recorder";

#define CALL_RECORDER_MOUNT_POINT "/sdcard"
#define CALL_RECORDER_DIR CALL_RECORDER_MOUNT_POINT "/calls"
#define CALL_RECORDER_QUEUE_LENGTH 24
#define CALL_RECORDER_MAX_SAMPLES 160
#define CALL_RECORDER_TASK_STACK_WORDS 4096
#define CALL_RECORDER_TASK_PRIORITY 3
#define CALL_RECORDER_WAV_CHANNELS 2
#define CALL_RECORDER_WAV_BITS_PER_SAMPLE 16

typedef struct {
    call_recorder_audio_dir_t dir;
    size_t sample_count;
    int16_t samples[CALL_RECORDER_MAX_SAMPLES];
} recorder_audio_frame_t;

typedef struct {
    char path[128];
} recorder_start_msg_t;

typedef enum {
    RECORDER_MSG_START,
    RECORDER_MSG_STOP,
    RECORDER_MSG_AUDIO,
} recorder_msg_type_t;

typedef struct {
    recorder_msg_type_t type;
    union {
        recorder_start_msg_t start;
        recorder_audio_frame_t audio;
    };
} recorder_msg_t;

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static sdmmc_card_t *s_card;
static BYTE s_pdrv = FF_DRV_NOT_USED;
static FATFS *s_fs;
static char s_fat_drive[8];
static bool s_ready;
static bool s_recording;
static uint32_t s_drop_count;

#define LOCAL_SDMMC_INIT_STEP(condition, function) \
    do { \
        if ((condition)) { \
            esp_err_t step_err = (function)(card); \
            if (step_err != ESP_OK) { \
                return step_err; \
            } \
        } \
    } while (0)

#define LOCAL_SDMMC_INIT_STEP_PARAM(condition, function, param) \
    do { \
        if ((condition)) { \
            esp_err_t step_err = (function)(card, param); \
            if (step_err != ESP_OK) { \
                return step_err; \
            } \
        } \
    } while (0)

static void configure_sd_gpio_pullups(void)
{
    gpio_set_pull_mode(BOARD_SD_MOSI_GPIO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(BOARD_SD_MISO_GPIO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(BOARD_SD_CS_GPIO, GPIO_PULLUP_ONLY);
    gpio_set_direction(BOARD_SD_CS_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_SD_CS_GPIO, 1);
}

static esp_err_t sdmmc_card_init_without_spi_crc(const sdmmc_host_t *config, sdmmc_card_t *card)
{
    memset(card, 0, sizeof(*card));
    memcpy(&card->host, config, sizeof(*config));

    const bool is_spi = host_is_spi(card);
    const bool always = true;
    const bool io_supported = true;

    LOCAL_SDMMC_INIT_STEP(always, sdmmc_check_host_function_ptr_integrity);
    LOCAL_SDMMC_INIT_STEP(always, sdmmc_io_reset);
    LOCAL_SDMMC_INIT_STEP(always, sdmmc_send_cmd_go_idle_state);
    LOCAL_SDMMC_INIT_STEP(always, sdmmc_init_sd_if_cond);
    LOCAL_SDMMC_INIT_STEP(io_supported, sdmmc_init_io);

    const bool is_mem = card->is_mem;
    const bool is_sdio = !is_mem;

    if (is_spi) {
        ESP_LOGW(TAG, "microSD SPI CRC enable step skipped");
    }

    LOCAL_SDMMC_INIT_STEP(is_mem, sdmmc_init_ocr);

    const bool is_mmc = is_mem && card->is_mmc;
    const bool is_sdmem = is_mem && !is_mmc;
    const bool is_uhs1 = is_sdmem && (card->ocr & SD_OCR_S18_RA) && (card->ocr & SD_OCR_SDHC_CAP);

    LOCAL_SDMMC_INIT_STEP(is_uhs1, sdmmc_init_sd_uhs1);
    LOCAL_SDMMC_INIT_STEP(is_mem, sdmmc_init_cid);
    LOCAL_SDMMC_INIT_STEP(!is_spi, sdmmc_init_rca);
    LOCAL_SDMMC_INIT_STEP(is_mem, sdmmc_init_csd);
    LOCAL_SDMMC_INIT_STEP(is_mmc && !is_spi, sdmmc_init_mmc_decode_cid);
    LOCAL_SDMMC_INIT_STEP(!is_spi, sdmmc_init_select_card);
    LOCAL_SDMMC_INIT_STEP(is_sdmem, sdmmc_init_sd_blocklen);
    LOCAL_SDMMC_INIT_STEP(is_sdmem, sdmmc_init_sd_scr);
    LOCAL_SDMMC_INIT_STEP(is_sdmem, sdmmc_init_sd_wait_data_ready);
    LOCAL_SDMMC_INIT_STEP(is_mmc, sdmmc_init_mmc_read_ext_csd);

    uint8_t card_cap = 0;
    LOCAL_SDMMC_INIT_STEP_PARAM(is_sdio, sdmmc_io_init_read_card_cap, &card_cap);
    LOCAL_SDMMC_INIT_STEP(always, sdmmc_init_card_hs_mode);

    if (!is_spi) {
        LOCAL_SDMMC_INIT_STEP(is_sdmem, sdmmc_init_sd_bus_width);
        LOCAL_SDMMC_INIT_STEP(is_sdio, sdmmc_init_io_bus_width);
        LOCAL_SDMMC_INIT_STEP(is_mmc, sdmmc_init_mmc_bus_width);
        LOCAL_SDMMC_INIT_STEP(always, sdmmc_init_host_bus_width);
    }

    LOCAL_SDMMC_INIT_STEP(is_uhs1, sdmmc_init_sd_driver_strength);
    LOCAL_SDMMC_INIT_STEP(is_uhs1, sdmmc_init_sd_current_limit);
    LOCAL_SDMMC_INIT_STEP(is_sdmem, sdmmc_init_sd_ssr);
    LOCAL_SDMMC_INIT_STEP(always, sdmmc_init_host_frequency);
    LOCAL_SDMMC_INIT_STEP(is_uhs1, sdmmc_init_sd_timing_tuning);
    LOCAL_SDMMC_INIT_STEP(is_sdmem, sdmmc_check_scr);
    LOCAL_SDMMC_INIT_STEP(is_mmc, sdmmc_init_mmc_check_ext_csd);
    LOCAL_SDMMC_INIT_STEP_PARAM(is_sdio, sdmmc_io_init_check_card_cap, &card_cap);

    return ESP_OK;
}

static void deinit_sdspi_device(const sdmmc_host_t *host_config)
{
    if ((host_config->flags & SDMMC_HOST_FLAG_DEINIT_ARG) != 0 && host_config->deinit_p != NULL) {
        host_config->deinit_p(host_config->slot);
    } else if (host_config->deinit != NULL) {
        host_config->deinit();
    }
}

static esp_err_t mount_sdspi_without_crc(const sdmmc_host_t *host_config_input,
                                         const sdspi_device_config_t *slot_config,
                                         const esp_vfs_fat_mount_config_t *mount_config,
                                         sdmmc_card_t **out_card)
{
    esp_err_t ret = ESP_OK;
    int card_handle = -1;
    bool device_attached = false;
    sdmmc_card_t *card = NULL;
    FATFS *fs = NULL;
    BYTE pdrv = FF_DRV_NOT_USED;
    char drive[8] = {0};

    ret = host_config_input->init();
    ESP_RETURN_ON_ERROR(ret, TAG, "SD SPI host init failed");

    ret = sdspi_host_init_device(slot_config, &card_handle);
    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "SD SPI attach failed");
    device_attached = true;

    sdmmc_host_t host_config = *host_config_input;
    host_config.slot = card_handle;

    card = calloc(1, sizeof(*card));
    ESP_GOTO_ON_FALSE(card != NULL, ESP_ERR_NO_MEM, cleanup, TAG, "allocate SD card descriptor failed");

    ret = sdmmc_card_init_without_spi_crc(&host_config, card);
    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "SD card init without SPI CRC failed");

    ret = ff_diskio_get_drive(&pdrv);
    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "get FATFS drive failed");
    ESP_GOTO_ON_FALSE(pdrv != FF_DRV_NOT_USED, ESP_ERR_NO_MEM, cleanup, TAG, "no free FATFS drive");

    ff_diskio_register_sdmmc(pdrv, card);
    ff_sdmmc_set_disk_status_check(pdrv, mount_config->disk_status_check_enable);
    snprintf(drive, sizeof(drive), "%u:", (unsigned)pdrv);

    esp_vfs_fat_conf_t conf = {
        .base_path = CALL_RECORDER_MOUNT_POINT,
        .fat_drive = drive,
        .max_files = mount_config->max_files,
    };
    ret = esp_vfs_fat_register_cfg(&conf, &fs);
    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "register FATFS VFS failed");

    FRESULT mount_result = f_mount(fs, drive, 1);
    ESP_GOTO_ON_FALSE(mount_result == FR_OK,
                      ESP_FAIL,
                      cleanup,
                      TAG,
                      "mount FATFS failed: %d",
                      mount_result);

    s_pdrv = pdrv;
    s_fs = fs;
    strlcpy(s_fat_drive, drive, sizeof(s_fat_drive));
    *out_card = card;
    return ESP_OK;

cleanup:
    if (fs != NULL) {
        f_mount(NULL, drive, 0);
        esp_vfs_fat_unregister_path(CALL_RECORDER_MOUNT_POINT);
    }
    if (pdrv != FF_DRV_NOT_USED) {
        ff_diskio_unregister(pdrv);
    }
    if (device_attached) {
        sdmmc_host_t host_config = *host_config_input;
        host_config.slot = card_handle;
        deinit_sdspi_device(&host_config);
    }
    free(card);
    return ret;
}

static void unmount_sdspi_without_crc(sdmmc_card_t *card)
{
    if (s_fs != NULL && s_fat_drive[0] != '\0') {
        f_mount(NULL, s_fat_drive, 0);
        esp_vfs_fat_unregister_path(CALL_RECORDER_MOUNT_POINT);
        s_fs = NULL;
        s_fat_drive[0] = '\0';
    }
    if (s_pdrv != FF_DRV_NOT_USED) {
        ff_diskio_unregister(s_pdrv);
        s_pdrv = FF_DRV_NOT_USED;
    }
    if (card != NULL) {
        deinit_sdspi_device(&card->host);
        free(card);
    }
}

static void write_le16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xff);
    dst[1] = (uint8_t)(value >> 8);
}

static void write_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xff);
    dst[1] = (uint8_t)((value >> 8) & 0xff);
    dst[2] = (uint8_t)((value >> 16) & 0xff);
    dst[3] = (uint8_t)((value >> 24) & 0xff);
}

static bool write_wav_header(FILE *file, uint32_t data_bytes)
{
    uint8_t header[44] = {0};
    const uint32_t sample_rate = CONFIG_IPPHONE_I2S_SAMPLE_RATE;
    const uint32_t byte_rate = sample_rate * CALL_RECORDER_WAV_CHANNELS * (CALL_RECORDER_WAV_BITS_PER_SAMPLE / 8);
    const uint16_t block_align = CALL_RECORDER_WAV_CHANNELS * (CALL_RECORDER_WAV_BITS_PER_SAMPLE / 8);

    memcpy(&header[0], "RIFF", 4);
    write_le32(&header[4], 36 + data_bytes);
    memcpy(&header[8], "WAVE", 4);
    memcpy(&header[12], "fmt ", 4);
    write_le32(&header[16], 16);
    write_le16(&header[20], 1);
    write_le16(&header[22], CALL_RECORDER_WAV_CHANNELS);
    write_le32(&header[24], sample_rate);
    write_le32(&header[28], byte_rate);
    write_le16(&header[32], block_align);
    write_le16(&header[34], CALL_RECORDER_WAV_BITS_PER_SAMPLE);
    memcpy(&header[36], "data", 4);
    write_le32(&header[40], data_bytes);

    return fwrite(header, 1, sizeof(header), file) == sizeof(header);
}

static bool patch_wav_header(FILE *file, uint32_t data_bytes)
{
    if (fseek(file, 0, SEEK_SET) != 0) {
        return false;
    }
    return write_wav_header(file, data_bytes);
}

static void sanitize_filename_part(const char *src, char *dst, size_t dst_len)
{
    if (dst_len == 0) {
        return;
    }

    if (src == NULL || src[0] == '\0') {
        strlcpy(dst, "unknown", dst_len);
        return;
    }

    size_t written = 0;
    for (size_t i = 0; src[i] != '\0' && written + 1 < dst_len; ++i) {
        const unsigned char ch = (unsigned char)src[i];
        if (isalnum(ch) || ch == '-' || ch == '_') {
            dst[written++] = (char)ch;
        } else {
            dst[written++] = '_';
        }
    }

    dst[written] = '\0';
    if (written == 0) {
        strlcpy(dst, "unknown", dst_len);
    }
}

static void make_recording_path(const char *caller, const char *callee, char *out, size_t out_len)
{
    char safe_caller[32] = {0};
    char safe_callee[32] = {0};
    sanitize_filename_part(caller, safe_caller, sizeof(safe_caller));
    sanitize_filename_part(callee, safe_callee, sizeof(safe_callee));

    time_t now = 0;
    time(&now);
    struct tm tm_info = {0};
    localtime_r(&now, &tm_info);

    char stamp[32] = {0};
    if ((tm_info.tm_year + 1900) >= 2024) {
        snprintf(stamp,
                 sizeof(stamp),
                 "%04d%02d%02d_%02d%02d%02d",
                 tm_info.tm_year + 1900,
                 tm_info.tm_mon + 1,
                 tm_info.tm_mday,
                 tm_info.tm_hour,
                 tm_info.tm_min,
                 tm_info.tm_sec);
    } else {
        snprintf(stamp, sizeof(stamp), "u%llu", (unsigned long long)(esp_timer_get_time() / 1000000ULL));
    }

    snprintf(out, out_len, CALL_RECORDER_DIR "/%s_%s_%s.wav", safe_caller, safe_callee, stamp);
}

static void write_stereo_frame(FILE *file,
                               const recorder_audio_frame_t *frame,
                               int16_t *pending_rx,
                               size_t *pending_rx_count,
                               int16_t *pending_tx,
                               size_t *pending_tx_count,
                               uint32_t *data_bytes)
{
    if (frame->dir == CALL_RECORDER_AUDIO_RX) {
        memcpy(pending_rx, frame->samples, frame->sample_count * sizeof(int16_t));
        *pending_rx_count = frame->sample_count;
    } else {
        memcpy(pending_tx, frame->samples, frame->sample_count * sizeof(int16_t));
        *pending_tx_count = frame->sample_count;
    }

    if (*pending_rx_count == 0 && *pending_tx_count == 0) {
        return;
    }

    if (*pending_rx_count == 0 || *pending_tx_count == 0) {
        return;
    }

    size_t count = *pending_rx_count < *pending_tx_count ? *pending_rx_count : *pending_tx_count;
    int16_t stereo[CALL_RECORDER_MAX_SAMPLES * CALL_RECORDER_WAV_CHANNELS] = {0};
    if (count > CALL_RECORDER_MAX_SAMPLES) {
        count = CALL_RECORDER_MAX_SAMPLES;
    }

    for (size_t i = 0; i < count; ++i) {
        stereo[i * 2] = pending_rx[i];
        stereo[(i * 2) + 1] = pending_tx[i];
    }

    const size_t bytes = count * CALL_RECORDER_WAV_CHANNELS * sizeof(int16_t);
    if (fwrite(stereo, 1, bytes, file) == bytes) {
        *data_bytes += (uint32_t)bytes;
    }

    *pending_rx_count = 0;
    *pending_tx_count = 0;
}

static void recorder_task(void *arg)
{
    (void)arg;

    FILE *file = NULL;
    uint32_t data_bytes = 0;
    int16_t pending_rx[CALL_RECORDER_MAX_SAMPLES] = {0};
    int16_t pending_tx[CALL_RECORDER_MAX_SAMPLES] = {0};
    size_t pending_rx_count = 0;
    size_t pending_tx_count = 0;
    recorder_msg_t msg = {0};

    while (true) {
        if (xQueueReceive(s_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (msg.type == RECORDER_MSG_START) {
            if (file != NULL) {
                patch_wav_header(file, data_bytes);
                fclose(file);
                file = NULL;
            }

            data_bytes = 0;
            pending_rx_count = 0;
            pending_tx_count = 0;
            errno = 0;
            file = fopen(msg.start.path, "wb");
            if (file == NULL) {
                ESP_LOGW(TAG, "open recording file failed: %s errno=%d", msg.start.path, errno);
                if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
                    s_recording = false;
                    xSemaphoreGive(s_lock);
                }
                continue;
            }

            if (!write_wav_header(file, 0)) {
                ESP_LOGW(TAG, "write WAV header failed");
                fclose(file);
                file = NULL;
                if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
                    s_recording = false;
                    xSemaphoreGive(s_lock);
                }
                continue;
            }

            ESP_LOGI(TAG, "call recording started: %s", msg.start.path);
        } else if (msg.type == RECORDER_MSG_STOP) {
            if (file != NULL) {
                if (!patch_wav_header(file, data_bytes)) {
                    ESP_LOGW(TAG, "patch WAV header failed");
                }
                fclose(file);
                file = NULL;
                ESP_LOGI(TAG, "call recording stopped, bytes=%lu", (unsigned long)data_bytes);
            }
        } else if (msg.type == RECORDER_MSG_AUDIO && file != NULL) {
            write_stereo_frame(file, &msg.audio, pending_rx, &pending_rx_count, pending_tx, &pending_tx_count, &data_bytes);
        }
    }
}

esp_err_t call_recorder_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_NO_MEM, TAG, "create recorder mutex failed");

    s_queue = xQueueCreate(CALL_RECORDER_QUEUE_LENGTH, sizeof(recorder_msg_t));
    ESP_RETURN_ON_FALSE(s_queue != NULL, ESP_ERR_NO_MEM, TAG, "create recorder queue failed");

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = CONFIG_IPPHONE_SD_SPI_FREQ_KHZ;

    configure_sd_gpio_pullups();

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = BOARD_SD_MOSI_GPIO,
        .miso_io_num = BOARD_SD_MISO_GPIO,
        .sclk_io_num = BOARD_SD_SCK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };

    esp_err_t err = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "initialize SD SPI bus failed: %s", esp_err_to_name(err));
        return err;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = BOARD_SD_CS_GPIO;
    slot_config.host_id = host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 2,
        .allocation_unit_size = 16 * 1024,
    };

    err = mount_sdspi_without_crc(&host, &slot_config, &mount_config, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "microSD mount failed: %s; call recording disabled", esp_err_to_name(err));
        return err;
    }

    errno = 0;
    if (mkdir(CALL_RECORDER_DIR, 0775) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "create recording directory failed: %s errno=%d", CALL_RECORDER_DIR, errno);
    }

    BaseType_t created = xTaskCreate(recorder_task,
                                     "call_recorder",
                                     CALL_RECORDER_TASK_STACK_WORDS,
                                     NULL,
                                     CALL_RECORDER_TASK_PRIORITY,
                                     &s_task);
    if (created != pdPASS) {
        unmount_sdspi_without_crc(s_card);
        s_card = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_ready = true;
    ESP_LOGI(TAG,
             "microSD ready for call recording with SPI CRC disabled: SCK=%d MOSI=%d MISO=%d CS=%d freq=%d kHz",
             BOARD_SD_SCK_GPIO,
             BOARD_SD_MOSI_GPIO,
             BOARD_SD_MISO_GPIO,
             BOARD_SD_CS_GPIO,
             CONFIG_IPPHONE_SD_SPI_FREQ_KHZ);
    return ESP_OK;
}

bool call_recorder_is_ready(void)
{
    return s_ready;
}

esp_err_t call_recorder_start(const char *caller, const char *callee)
{
    if (!s_ready || s_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    recorder_msg_t msg = {
        .type = RECORDER_MSG_START,
    };
    make_recording_path(caller, callee, msg.start.path, sizeof(msg.start.path));

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_recording = true;
    s_drop_count = 0;
    xSemaphoreGive(s_lock);

    if (xQueueSend(s_queue, &msg, pdMS_TO_TICKS(200)) != pdTRUE) {
        if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
            s_recording = false;
            xSemaphoreGive(s_lock);
        }
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

void call_recorder_stop(void)
{
    if (!s_ready || s_queue == NULL) {
        return;
    }

    bool was_recording = false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
        was_recording = s_recording;
        s_recording = false;
        xSemaphoreGive(s_lock);
    }

    if (!was_recording) {
        return;
    }

    recorder_msg_t msg = {
        .type = RECORDER_MSG_STOP,
    };
    xQueueSend(s_queue, &msg, pdMS_TO_TICKS(200));
}

void call_recorder_push_audio(call_recorder_audio_dir_t dir, const int16_t *samples, size_t sample_count)
{
    if (!s_ready || s_queue == NULL || samples == NULL || sample_count == 0) {
        return;
    }

    bool recording = false;
    if (xSemaphoreTake(s_lock, 0) == pdTRUE) {
        recording = s_recording;
        xSemaphoreGive(s_lock);
    }
    if (!recording) {
        return;
    }

    recorder_msg_t msg = {
        .type = RECORDER_MSG_AUDIO,
    };
    msg.audio.dir = dir;
    msg.audio.sample_count = sample_count > CALL_RECORDER_MAX_SAMPLES ? CALL_RECORDER_MAX_SAMPLES : sample_count;
    memcpy(msg.audio.samples, samples, msg.audio.sample_count * sizeof(int16_t));

    if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
        ++s_drop_count;
        if ((s_drop_count % 50) == 1) {
            ESP_LOGW(TAG, "recording queue full, dropped frames=%lu", (unsigned long)s_drop_count);
        }
    }
}
