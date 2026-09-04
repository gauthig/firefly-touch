idf_component_register(
    SRCS "valve_control.c" "valve_control_driver.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_timer
)
