idf_component_register(
    SRCS "water_quality.c"
    INCLUDE_DIRS "include"
    REQUIRES sen0708 sensor_health telemetry_hub hardware_map freertos esp_timer log
)
