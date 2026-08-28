idf_component_register(
    SRCS "ui_theme.c" "ui_dimmer_button.c" "ui_tank_wave.c" "ui_battery_summary.c"
         "ui_shore_panel.c" "ui_solar_panel.c"
    INCLUDE_DIRS "include"
    # renogy_solar for the same reason as jbd_bms: the pure-C protocol half
    # (types, renogy_charge_state_str) is what the readout renders. Its BLE
    # client compiles to stubs on a panel -- see renogy_solar_client.c.
    REQUIRES rvc_protocol jbd_bms renogy_solar
)
