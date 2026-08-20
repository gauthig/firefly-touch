idf_component_register(
    SRCS "jbd_bms_protocol.c" "jbd_bms_client.c"
    INCLUDE_DIRS "include"
    REQUIRES bt nvs_flash
)
