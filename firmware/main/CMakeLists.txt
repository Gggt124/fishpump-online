if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    set(STATIC_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/static")
    set(STATIC_OUT_DIR "${CMAKE_CURRENT_LIST_DIR}/static_compressed")
    file(MAKE_DIRECTORY "${STATIC_OUT_DIR}")

    add_custom_command(
        OUTPUT
            "${STATIC_OUT_DIR}/index.html"
            "${STATIC_OUT_DIR}/index.html.gz"
            "${STATIC_OUT_DIR}/style.css"
            "${STATIC_OUT_DIR}/style.css.gz"
            "${STATIC_OUT_DIR}/app.js"
            "${STATIC_OUT_DIR}/app.js.gz"
        COMMAND python "${CMAKE_CURRENT_LIST_DIR}/../scripts/optimize_assets.py"
        DEPENDS
            "${STATIC_SRC_DIR}/index.html"
            "${STATIC_SRC_DIR}/style.css"
            "${STATIC_SRC_DIR}/app.js"
            "${CMAKE_CURRENT_LIST_DIR}/../scripts/optimize_assets.py"
        COMMENT "Minifying and compressing static web assets..."
        VERBATIM
    )
    add_custom_target(generate_static_assets
        DEPENDS
            "${STATIC_OUT_DIR}/index.html.gz"
            "${STATIC_OUT_DIR}/style.css.gz"
            "${STATIC_OUT_DIR}/app.js.gz"
    )
endif()

idf_component_register(
    SRCS
        "app_main.c"
        "web_server.c"
    INCLUDE_DIRS
        "."
    EMBED_FILES
        "static_compressed/index.html"
        "static_compressed/index.html.gz"
        "static_compressed/style.css"
        "static_compressed/style.css.gz"
        "static_compressed/app.js"
        "static_compressed/app.js.gz"
        "static/ibm_plex_sans_thai_400_latin.woff2"
        "static/ibm_plex_sans_thai_400_thai.woff2"
        "static/ibm_plex_sans_thai_700_latin.woff2"
        "static/ibm_plex_sans_thai_700_thai.woff2"
        "static/jetbrains_mono_400_latin.woff2"
    REQUIRES
        app_update
        app_config
        cooling_control
        dns_server
        hardware_map
        nvs_flash
        nvs_store
        pump_control
        relay_safe_init
        session
        telemetry_hub
        tft_display
        wifi_manager
        esp_http_server
        esp_wifi
        cjson
        mdns
        mbedtls
        led_strip
)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    add_dependencies(${COMPONENT_LIB} generate_static_assets)
endif()
