idf_component_register(
    SRCS "selftest_main.c" "test_sensor_health.c"
    INCLUDE_DIRS "."
    REQUIRES sensor_health log
)
