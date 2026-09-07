idf_component_register(
    SRCS "ui_theme.c" "ui_dimmer_button.c" "ui_tank_wave.c" "ui_battery_summary.c"
         "ui_shore_panel.c" "ui_solar_panel.c" "ui_thermostat.c"
    INCLUDE_DIRS "include"
    # renogy_solar for the same reason as jbd_bms: the pure-C protocol half
    # (types, renogy_charge_state_str) is what the readout renders. Its BLE
    # client compiles to stubs on a panel -- see renogy_solar_client.c.
    # easytouch: ui_thermostat renders easytouch_status_t and builds
    # easytouch_change_t; the BLE client symbol is referenced only from
    # main/ui/ui.c, not here.
    REQUIRES rvc_protocol jbd_bms renogy_solar easytouch
)

# ui_metrics.h picks a large-display variant when UI_METRICS_LARGE is defined.
# BOARD is derived from PANEL in the root CMakeLists.txt and "lcd7b" maps only
# to main_cabinet (the Waveshare 7B, 1024x600); every other panel keeps the
# byte-identical small variant. PUBLIC so main/ (ui.c) sees it too -- main
# REQUIRES ui_common. Same pattern components/board/CMakeLists.txt uses to
# read BOARD in the real (not early-expansion) registration pass.
if(BOARD STREQUAL "lcd7b")
    target_compile_definitions(${COMPONENT_LIB} PUBLIC UI_METRICS_LARGE=1)
endif()
