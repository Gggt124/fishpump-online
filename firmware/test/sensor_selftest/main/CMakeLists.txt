idf_component_register(
    SRCS "selftest_main.c" "test_sensor_health.c" "test_sen0708.c" "test_telemetry.c" "test_contact_debounce.c" "test_signal_registry.c" "test_alarm_rules.c"
    INCLUDE_DIRS "."
    REQUIRES sensor_health sen0708 pzem004t telemetry_hub contact_debounce signal_registry alarm_rules cjson log
)
