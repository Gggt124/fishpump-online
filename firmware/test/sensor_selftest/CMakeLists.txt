# Standalone QEMU self-test for the sensor pure-C logic.
# Build: idf.py -C firmware/test/sensor_selftest build
# Run:   idf.py -C firmware/test/sensor_selftest qemu   -> "SELFTEST PASS (N checks)"
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS
    ../../components/sensor_health
    ../../components/sen0708
    ../../components/pzem004t  # compile check only (no pure logic to test)
    ../../components/telemetry_hub
)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(sensor_selftest)
