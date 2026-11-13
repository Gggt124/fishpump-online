idf_component_register(
    SRCS "selftest_main.c" "test_sensor_health.c" "test_sen0708.c"
    INCLUDE_DIRS "."
    REQUIRES sensor_health sen0708 log
)
