idf_component_register(
    SRCS "hardware_map.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio
    PRIV_REQUIRES nvs_store
)
