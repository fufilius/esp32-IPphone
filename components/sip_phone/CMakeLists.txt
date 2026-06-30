idf_component_register(SRCS "sip_phone.c"
                       INCLUDE_DIRS "include"
                       REQUIRES audio_hw esp_common esp_hw_support esp_netif freertos lwip mbedtls)
