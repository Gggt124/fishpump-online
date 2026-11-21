idf_component_register(
    SRCS "sen0708_proto.c" "ph_cal.c" "sen0708.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_uart esp_timer freertos
)
