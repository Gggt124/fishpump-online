idf_component_register(
    SRCS "session.c"
    INCLUDE_DIRS "include"
    REQUIRES app_config esp_system esp_timer freertos
)
