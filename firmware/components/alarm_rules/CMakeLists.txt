idf_component_register(
    SRCS "alarm_validate.c" "alarm_json.c"
    INCLUDE_DIRS "include"
    REQUIRES signal_registry telemetry_hub cjson
)
