idf_component_register(
    SRCS "ui_theme.c" "ui_dimmer_button.c" "ui_tank_wave.c" "ui_battery_summary.c"
         "ui_shore_panel.c"
    INCLUDE_DIRS "include"
    REQUIRES rvc_protocol jbd_bms
)
