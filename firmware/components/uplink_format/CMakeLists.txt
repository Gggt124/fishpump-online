idf_component_register(
    SRCS "uplink_format.c"
    INCLUDE_DIRS "include"
    REQUIRES signal_registry alarm_rules telemetry_hub cjson
)
