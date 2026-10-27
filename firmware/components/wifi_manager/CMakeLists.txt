idf_component_register(
    SRCS "wifi_manager.c"
    INCLUDE_DIRS "include" "."
    REQUIRES
        app_config
        hardware_map
        nvs_store
        esp_wifi
        esp_event
        esp_netif
        esp_timer
        freertos
)
