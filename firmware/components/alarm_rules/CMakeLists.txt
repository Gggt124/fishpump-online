idf_component_register(
    SRCS "alarm_validate.c" "alarm_json.c" "alarm_eval.c" "alarm_ring.c" "alarm_presets.c" "alarm_blob.c"
    INCLUDE_DIRS "include"
    REQUIRES signal_registry telemetry_hub cjson
)
