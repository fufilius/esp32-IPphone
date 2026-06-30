#pragma once

#include <stdbool.h>

#include "esp_err.h"

bool ipphone_wifi_is_configured(void);
esp_err_t ipphone_wifi_start(void);
