idf_component_register(
    SRCS "flow_switch.c"
    INCLUDE_DIRS "include"
    REQUIRES contact_debounce telemetry_hub hardware_map esp_driver_gpio freertos esp_timer log
)
