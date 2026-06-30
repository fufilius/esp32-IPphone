idf_component_register(SRCS "audio_hw.c"
                       INCLUDE_DIRS "include"
                       REQUIRES board driver esp_common)

target_link_libraries(${COMPONENT_LIB} INTERFACE m)
