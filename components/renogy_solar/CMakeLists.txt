idf_component_register(
    SRCS "renogy_solar_protocol.c" "renogy_solar_client.c"
    INCLUDE_DIRS "include"
    REQUIRES ble_host bt
)
