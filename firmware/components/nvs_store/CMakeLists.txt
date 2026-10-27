idf_component_register(
    SRCS "nvs_store.c"
    INCLUDE_DIRS "include"
    REQUIRES app_config hardware_map nvs_flash
)
