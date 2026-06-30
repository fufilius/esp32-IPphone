idf_component_register(SRCS "sip_phone.c"
                       INCLUDE_DIRS "include"
                       REQUIRES audio_hw call_recorder esp_common esp_hw_support esp_netif freertos lwip mbedtls)
