# Release builds must carry a real OTA key, never the all-zero dev fallback.
#
# Used two ways:
#   - included from firmware/CMakeLists.txt when -DFISHPUMP_RELEASE=ON
#   - checked on its own:  cmake -DSECRETS_FILE=<path> -P check_release_key.cmake
#
# The key itself is never printed.

if(NOT DEFINED SECRETS_FILE)
    message(FATAL_ERROR "check_release_key: SECRETS_FILE is not set")
endif()

set(_fix "Create it with:  python firmware/scripts/write_secrets.py --generate   (or --from-env NAME in CI). See docs/release/keys.md.")

if(NOT EXISTS "${SECRETS_FILE}")
    message(FATAL_ERROR "Release build needs the OTA key file, but ${SECRETS_FILE} does not exist.\n${_fix}")
endif()

file(READ "${SECRETS_FILE}" _secrets_text)
string(REGEX MATCH "APP_CONFIG_OTA_ENCRYPTION_KEY[ \t]+\"([^\"]*)\"" _unused "${_secrets_text}")
set(_key "${CMAKE_MATCH_1}")

string(LENGTH "${_key}" _key_len)
string(REGEX MATCH "[^0-9a-fA-F]" _non_hex "${_key}")
string(REGEX MATCH "[^0]" _non_zero "${_key}")

if(NOT _key_len EQUAL 64 OR NOT "${_non_hex}" STREQUAL "")
    message(FATAL_ERROR "Release build: the OTA key in ${SECRETS_FILE} is not 64 hex characters (found ${_key_len} characters).\n${_fix}")
endif()
if("${_non_zero}" STREQUAL "")
    message(FATAL_ERROR "Release build: the OTA key in ${SECRETS_FILE} is all zeros (the unconfigured dev key).\n${_fix}")
endif()

unset(_key)
unset(_secrets_text)
