idf_component_register(
    SRCS "tb_link.c"
    INCLUDE_DIRS "include"
    REQUIRES mqtt tb_link_logic nvs_store uplink wifi_manager esp_wifi esp_event mbedtls freertos log
)
