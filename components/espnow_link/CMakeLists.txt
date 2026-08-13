idf_component_register(
    SRCS "espnow_link.c"
    INCLUDE_DIRS "include"
    REQUIRES rvc_protocol esp_wifi esp_event esp_netif nvs_flash
)
