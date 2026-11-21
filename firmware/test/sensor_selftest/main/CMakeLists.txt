idf_component_register(
    SRCS "selftest_main.c" "test_sensor_health.c" "test_sen0708.c" "test_telemetry.c" "test_contact_debounce.c" "test_signal_registry.c" "test_alarm_rules.c" "test_alarm_eval.c" "test_alarm_store.c" "test_uplink_format.c" "test_uplink_state.c" "test_dry_run.c" "test_tb_link.c" "test_tb_fw.c" "test_device_api.c" "test_dapi_guards.c"
    INCLUDE_DIRS "."
    REQUIRES sensor_health sen0708 pzem004t telemetry_hub contact_debounce signal_registry alarm_rules uplink_format dry_run_logic tb_link_logic device_api dapi_guards cjson log
)
