cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS "${CMAKE_SOURCE_DIR}/components")
set(SDKCONFIG_DEFAULTS "${CMAKE_SOURCE_DIR}/sdkconfig.defaults")

file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/version.txt" FISHPUMP_VERSION LIMIT_COUNT 1)
set(PROJECT_VER "${FISHPUMP_VERSION}")
option(TB_TEST_BLOCK_CONNECT "Test image that never connects to ThingsBoard (OTA rollback bench)" OFF)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Before project(): IDF adds the components during project(), so a plain add_compile_definitions would miss them.
idf_build_set_property(COMPILE_DEFINITIONS "APP_TEMPLATE_FIRMWARE_VERSION=\"v${FISHPUMP_VERSION}\"" APPEND)
if(TB_TEST_BLOCK_CONNECT)
    idf_build_set_property(COMPILE_DEFINITIONS "TB_LINK_TEST_BLOCK_CONNECT=1" APPEND)
endif()
project(fishpump_firmware VERSION ${FISHPUMP_VERSION})
