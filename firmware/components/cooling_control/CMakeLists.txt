idf_component_register(
    SRCS "cooling_control.c" "cooling_rule_engine.c" "cooling_sensor_pt100.c"
    INCLUDE_DIRS "include" "."
    REQUIRES app_config hardware_map esp_driver_gpio esp_driver_spi esp_timer freertos onewire_bus ds18b20 esp-idf-lib__max31865 esp_system relay_safe_init telemetry_hub
)

