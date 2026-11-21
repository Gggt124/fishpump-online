idf_component_register(
    SRCS "power_monitor.c"
    INCLUDE_DIRS "include"
    REQUIRES pzem004t sensor_health telemetry_hub hardware_map freertos esp_timer log
)
