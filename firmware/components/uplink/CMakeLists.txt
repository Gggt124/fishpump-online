idf_component_register(
    SRCS "uplink.c"
    INCLUDE_DIRS "include"
    REQUIRES uplink_format alarm_engine alarm_rules signal_registry telemetry_hub app_config freertos esp_timer heap log cjson
)
