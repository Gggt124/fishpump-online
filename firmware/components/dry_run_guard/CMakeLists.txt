idf_component_register(
    SRCS "dry_run_guard.c"
    INCLUDE_DIRS "include"
    REQUIRES dry_run_logic alarm_engine alarm_rules pump_control flow_switch nvs_store freertos esp_timer log
)
