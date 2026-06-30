idf_component_register(SRCS "app_main.c"
                       INCLUDE_DIRS "."
                       REQUIRES board audio_hw button status_leds sip_phone ipphone_wifi nvs_flash)
