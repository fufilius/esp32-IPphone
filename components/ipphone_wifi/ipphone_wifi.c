#include "ipphone_wifi.h"

#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "sdkconfig.h"

static const char *TAG = "ipphone_wifi";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define WIFI_MAX_TX_POWER_QDBM 78
#define WIFI_MIN_USABLE_RSSI_DBM -82

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_count;
static bool s_started;

bool ipphone_wifi_is_configured(void)
{
    return strlen(CONFIG_IPPHONE_WIFI_SSID) > 0;
}

static void configure_wifi_tx_power(void)
{
    esp_err_t err = esp_wifi_set_max_tx_power(WIFI_MAX_TX_POWER_QDBM);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set Wi-Fi TX power failed: %s", esp_err_to_name(err));
        return;
    }

    int8_t tx_power = 0;
    if (esp_wifi_get_max_tx_power(&tx_power) == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi TX power limit set to %.2f dBm", (double)tx_power * 0.25);
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        configure_wifi_tx_power();
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = (const wifi_event_sta_disconnected_t *)event_data;
        if (s_retry_count < CONFIG_IPPHONE_WIFI_MAX_RETRY) {
            s_retry_count++;
            esp_wifi_connect();
            ESP_LOGW(TAG,
                     "Wi-Fi disconnected, reason=%d, retry %d/%d",
                     event->reason,
                     s_retry_count,
                     CONFIG_IPPHONE_WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        wifi_ap_record_t ap_info = {0};
        esp_err_t ap_err = esp_wifi_sta_get_ap_info(&ap_info);
        if (ap_err == ESP_OK) {
            ESP_LOGI(TAG,
                     "Wi-Fi connected, ip=" IPSTR ", netmask=" IPSTR ", gw=" IPSTR
                     ", rssi=%d dBm, channel=%u, bssid=%02x:%02x:%02x:%02x:%02x:%02x",
                     IP2STR(&event->ip_info.ip),
                     IP2STR(&event->ip_info.netmask),
                     IP2STR(&event->ip_info.gw),
                     ap_info.rssi,
                     ap_info.primary,
                     ap_info.bssid[0],
                     ap_info.bssid[1],
                     ap_info.bssid[2],
                     ap_info.bssid[3],
                     ap_info.bssid[4],
                     ap_info.bssid[5]);
            if (ap_info.rssi < WIFI_MIN_USABLE_RSSI_DBM) {
                ESP_LOGW(TAG, "Wi-Fi RSSI is weak for RTP audio: %d dBm", ap_info.rssi);
            }
        } else {
            ESP_LOGI(TAG,
                     "Wi-Fi connected, ip=" IPSTR ", netmask=" IPSTR ", gw=" IPSTR,
                     IP2STR(&event->ip_info.ip),
                     IP2STR(&event->ip_info.netmask),
                     IP2STR(&event->ip_info.gw));
        }
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t create_default_event_loop_once(void)
{
    esp_err_t err = esp_event_loop_create_default();
    return err == ESP_ERR_INVALID_STATE ? ESP_OK : err;
}

esp_err_t ipphone_wifi_start(void)
{
    ESP_RETURN_ON_FALSE(ipphone_wifi_is_configured(), ESP_ERR_INVALID_STATE, TAG, "Wi-Fi SSID is empty");

    if (s_started) {
        ESP_LOGI(TAG, "Wi-Fi already started");
        return ESP_OK;
    }

    s_wifi_event_group = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_wifi_event_group != NULL, ESP_ERR_NO_MEM, TAG, "create Wi-Fi event group failed");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init failed");
    ESP_RETURN_ON_ERROR(create_default_event_loop_once(), TAG, "create event loop failed");
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(sta_netif != NULL, ESP_FAIL, TAG, "create default Wi-Fi STA netif failed");
    ESP_RETURN_ON_ERROR(esp_netif_set_hostname(sta_netif, CONFIG_IPPHONE_WIFI_HOSTNAME), TAG, "set hostname failed");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init failed");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT,
                                                           ESP_EVENT_ANY_ID,
                                                           &wifi_event_handler,
                                                           NULL,
                                                           NULL),
                        TAG,
                        "register Wi-Fi event handler failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT,
                                                           IP_EVENT_STA_GOT_IP,
                                                           &wifi_event_handler,
                                                           NULL,
                                                           NULL),
                        TAG,
                        "register IP event handler failed");

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, CONFIG_IPPHONE_WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, CONFIG_IPPHONE_WIFI_PASSWORD, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi_config.sta.threshold.authmode = strlen(CONFIG_IPPHONE_WIFI_PASSWORD) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi_config.sta.threshold.rssi = WIFI_MIN_USABLE_RSSI_DBM;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set Wi-Fi mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "set Wi-Fi config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N),
                        TAG,
                        "set Wi-Fi protocol failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20), TAG, "set Wi-Fi bandwidth failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "disable Wi-Fi power save failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start Wi-Fi failed");

    ESP_LOGI(TAG, "connecting to Wi-Fi SSID \"%s\"", CONFIG_IPPHONE_WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(CONFIG_IPPHONE_WIFI_CONNECT_TIMEOUT_MS));

    if ((bits & WIFI_CONNECTED_BIT) != 0) {
        s_started = true;
        return ESP_OK;
    }

    if ((bits & WIFI_FAIL_BIT) != 0) {
        ESP_LOGE(TAG, "Wi-Fi connect failed after %d retries", CONFIG_IPPHONE_WIFI_MAX_RETRY);
        return ESP_FAIL;
    }

    ESP_LOGE(TAG, "Wi-Fi connect timeout after %d ms", CONFIG_IPPHONE_WIFI_CONNECT_TIMEOUT_MS);
    return ESP_ERR_TIMEOUT;
}
