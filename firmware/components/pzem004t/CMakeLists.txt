idf_component_register(
    SRCS "pzem004t.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_uart esp_timer freertos
)
