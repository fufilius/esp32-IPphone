#include "board.h"

#include "board_pins.h"
#include "esp_log.h"

static const char *TAG = "board";

esp_err_t board_log_pinout(void)
{
    ESP_LOGI(TAG, "PTT-compatible pinout:");
    ESP_LOGI(TAG, "button GPIO%d active=%d", BOARD_BUTTON_GPIO, BOARD_BUTTON_ACTIVE_LEVEL);
    ESP_LOGI(TAG,
             "mic I2S BCLK=GPIO%d WS=GPIO%d DIN=GPIO%d",
             BOARD_I2S_MIC_BCLK,
             BOARD_I2S_MIC_WS,
             BOARD_I2S_MIC_DIN);
    ESP_LOGI(TAG,
             "speaker I2S BCLK=GPIO%d WS=GPIO%d DOUT=GPIO%d",
             BOARD_I2S_SPK_BCLK,
             BOARD_I2S_SPK_WS,
             BOARD_I2S_SPK_DOUT);
    ESP_LOGI(TAG, "I2C SDA=GPIO%d SCL=GPIO%d", BOARD_I2C_SDA, BOARD_I2C_SCL);
    ESP_LOGI(TAG, "LED green=GPIO%d red=GPIO%d", BOARD_LED_GREEN_GPIO, BOARD_LED_RED_GPIO);
    ESP_LOGI(TAG,
             "microSD SPI SCK=GPIO%d MOSI=GPIO%d MISO=GPIO%d CS=GPIO%d",
             BOARD_SD_SCK_GPIO,
             BOARD_SD_MOSI_GPIO,
             BOARD_SD_MISO_GPIO,
             BOARD_SD_CS_GPIO);
    return ESP_OK;
}
