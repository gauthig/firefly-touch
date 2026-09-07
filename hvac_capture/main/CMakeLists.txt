idf_component_register(
    SRCS "easytouch_capture_main.c"
    INCLUDE_DIRS "."
    REQUIRES ble_host
    PRIV_REQUIRES nvs_flash esp_timer
)
