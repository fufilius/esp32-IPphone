idf_component_register(SRCS "call_recorder.c"
                       INCLUDE_DIRS "include"
                       REQUIRES board esp_timer fatfs sdmmc esp_driver_spi esp_driver_sdspi)
