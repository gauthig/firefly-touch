idf_component_register(
    SRCS "gateway_main.c"
    INCLUDE_DIRS "."
    REQUIRES ble_host bus_survey rvc_protocol bt driver esp_timer nvs_flash spi_flash
)
