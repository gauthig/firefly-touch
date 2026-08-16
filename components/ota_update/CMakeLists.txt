idf_component_register(
    SRCS "ota_manifest.c" "ota_update.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event esp_http_client esp_https_ota esp-tls app_update nvs_flash
)
