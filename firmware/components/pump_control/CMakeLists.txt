idf_component_register(
    SRCS "pump_control.c"
    INCLUDE_DIRS "include" "."
    REQUIRES app_config esp_driver_gpio esp_timer freertos nvs_flash relay_safe_init telemetry_hub
)
