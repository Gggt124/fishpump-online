idf_component_register(
    SRCS "alarm_engine.c"
    INCLUDE_DIRS "include"
    REQUIRES alarm_rules signal_registry telemetry_hub nvs_store freertos esp_timer esp_hw_support log
)
