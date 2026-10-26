cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS "${CMAKE_SOURCE_DIR}/components")
set(SDKCONFIG_DEFAULTS "${CMAKE_SOURCE_DIR}/sdkconfig.defaults")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(fishpump_firmware VERSION 0.3.2)

add_compile_definitions(APP_TEMPLATE_FIRMWARE_VERSION="v${PROJECT_VERSION}")
