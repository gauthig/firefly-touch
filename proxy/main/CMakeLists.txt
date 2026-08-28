idf_component_register(
    SRCS "proxy_main.c"
    INCLUDE_DIRS "."
    REQUIRES espnow_link hughes_watchdog jbd_bms renogy_solar
)
