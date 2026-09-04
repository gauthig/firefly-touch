idf_component_register(
    SRCS "valves_main.c"
    INCLUDE_DIRS "."
    REQUIRES valve_control driver
)
