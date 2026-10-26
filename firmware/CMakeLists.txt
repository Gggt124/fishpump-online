cmake_minimum_required(VERSION 3.22)

# Components directory within firmware folder
set(EXTRA_COMPONENT_DIRS "${CMAKE_SOURCE_DIR}/components")

# ESP32-S3 dedicated default configurations
set(SDKCONFIG_DEFAULTS "${CMAKE_SOURCE_DIR}/sdkconfig.defaults")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(fishpump_firmware)
