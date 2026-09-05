idf_component_register(
    SRCS "valves_main.c" "valve_drive.c"
    INCLUDE_DIRS "."
    REQUIRES valve_control driver espnow_link
)
