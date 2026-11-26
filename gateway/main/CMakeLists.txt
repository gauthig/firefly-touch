idf_component_register(
    SRCS "gateway_main.c"
    INCLUDE_DIRS "."
    REQUIRES ble_host bus_survey rvc_protocol easytouch hughes_watchdog jbd_bms renogy_solar bt driver esp_timer nvs_flash spi_flash
)
