/*
 * Panel: main_cabinet (on-screen name "MAIN CABINET") — the coach's main
 * cabinet touchscreen.
 * Build: idf.py -B build_main_cabinet -DPANEL=main_cabinet build
 *
 * FIRST PANEL ON A DIFFERENT BOARD: a Waveshare ESP32-S3-Touch-LCD-7
 * (non-B), not the 4.3B the other three use. The root CMakeLists.txt maps
 * PANEL -> BOARD, so nothing here selects it; see components/board_lcd7 for
 * why that mapping matters (CAN moves to GPIO20/19, where the 4.3B has
 * RS485, and it is muxed against native USB).
 *
 * FIRST PANEL WITH A SIDE-NAV RAIL: instead of swapping the whole screen,
 * a persistent left rail lists the sections and the selected one fills the
 * rest. That is what the 7" landscape display buys — the 4.3B panels are
 * portrait and have no width to give up.
 *
 * Sections (rail order is independent of screen index, so the button grid
 * can stay on screen 0 where build_button_grid() handles it):
 *
 *   POWER  -> screen 1   battery bank + shore power   (PANEL_DEFAULT_SCREEN)
 *   TANKS  -> screen 2   fresh/grey/black + valves
 *   LIGHTS -> screen 0   the button grid
 *
 * Instances come from docs/instance_map.yaml. Loads it lists as `switch`
 * (cargo, both awnings, the hitch pair) are PANEL_BTN_SWITCH here rather
 * than dimmers — ramping a relay-driven load does nothing useful.
 */
#pragma once

#include "lvgl.h"
#include "panel_def.h"

#define PANEL_NAME  "MAIN CABINET"
#define PANEL_INDEX 3

/* Hardwired to the RV-C bus (source address 0x83 = 0x80 + 3). */

/* Battery and shore-power readings exist only as ESP-NOW broadcasts from
 * the basement BLE proxy, so this panel listens on the broadcast channel
 * even though it has its own bus. Receive-only: no peer, no keys. */
#define PANEL_WANTS_TELEMETRY 1

#define PANEL_HAS_SCREEN_2 1
#define PANEL_HAS_SCREEN_3 1
#define PANEL_HAS_NAV_RAIL 1
#define PANEL_DEFAULT_SCREEN 1   /* boot into POWER */
#define PANEL_GRID_COLS 3        /* 800 px wide minus the rail fits three */

/*
 * The rail. Ordinary PANEL_BTN_SCREEN_SWITCH entries — instances[0] names
 * the target screen, exactly as on the other panels' nav buttons.
 */
static const panel_btn_def_t PANEL_NAV_RAIL[] = {
    { .label = "POWER", .type = PANEL_BTN_SCREEN_SWITCH, .instances = {1}, .instance_count = 1 },
    { .label = "TANKS", .type = PANEL_BTN_SCREEN_SWITCH, .instances = {2}, .instance_count = 1 },
    { .label = "LIGHTS", .type = PANEL_BTN_SCREEN_SWITCH, .instances = {0}, .instance_count = 1 },
};

#define PANEL_NAV_RAIL_COUNT (sizeof(PANEL_NAV_RAIL) / sizeof(PANEL_NAV_RAIL[0]))

/*
 * Screen 0 — LIGHTS. Three columns x four rows; MASTER leads.
 */
static const panel_btn_def_t PANEL_BUTTONS[] = {
    { .label = "MASTER", .type = PANEL_BTN_LIGHT_MASTER, .instances = {0}, .instance_count = 0 },
    { .label = "ENTRY CEILING", .type = PANEL_BTN_DIMMER, .instances = {24}, .instance_count = 1 },
    { .label = "PORCH", .type = PANEL_BTN_DIMMER, .instances = {42}, .instance_count = 1 },
    { .label = "CARGO", .type = PANEL_BTN_SWITCH, .instances = {43}, .instance_count = 1 },
    { .label = "UNDER SLIDE", .type = PANEL_BTN_DIMMER, .instances = {37}, .instance_count = 1 },
    { .label = "AWNING DS", .type = PANEL_BTN_SWITCH, .instances = {38}, .instance_count = 1 },
    { .label = "AWNING PS", .type = PANEL_BTN_SWITCH, .instances = {39}, .instance_count = 1 },
    { .label = "ACCENT", .type = PANEL_BTN_DIMMER, .instances = {26, 27}, .instance_count = 2 },
    { .label = "MIDSHIP", .type = PANEL_BTN_DIMMER, .instances = {35}, .instance_count = 1 },
    { .label = "BEDROOM CEILING", .type = PANEL_BTN_DIMMER, .instances = {17}, .instance_count = 1 },
    { .label = "SECURITY P+H", .type = PANEL_BTN_SWITCH, .instances = {44, 45}, .instance_count = 2 },
};

#define PANEL_BUTTON_COUNT (sizeof(PANEL_BUTTONS) / sizeof(PANEL_BUTTONS[0]))

/*
 * What MASTER turns ON. RV-C has no all-on command and the G6's own LIGHT
 * MASTER DGN has never been captured (docs/instance_map.yaml records the
 * factory rocker as having no instance number), so "on" is this declared
 * scene rather than a broadcast. Turning OFF needs no list: ui.c sweeps
 * every instance the state manager currently reports as on, which reaches
 * lights this panel has no button for.
 */
static const uint8_t PANEL_MASTER_ON[] = { 24, 26, 27, 35, 13, 17 };

#define PANEL_MASTER_ON_COUNT (sizeof(PANEL_MASTER_ON) / sizeof(PANEL_MASTER_ON[0]))

/*
 * Screen 1 — POWER. The battery bank (combined across the three parallel
 * packs) beside the Hughes Power Watchdog's Line 1 / Line 2 readout. Both
 * arrive as broadcasts; neither takes an RV-C instance.
 */
static const panel_btn_def_t PANEL_BUTTONS_2[] = {
    { .label = "BANK", .type = PANEL_BTN_BATTERY_SUMMARY, .instances = {0}, .instance_count = 0 },
    { .label = "SHORE", .type = PANEL_BTN_SHORE_POWER, .instances = {0}, .instance_count = 0 },
};

#define PANEL_BUTTON_COUNT_2 (sizeof(PANEL_BUTTONS_2) / sizeof(PANEL_BUTTONS_2[0]))

/*
 * Screen 2 — TANKS. The three SeeLevel gauges, plus three valve controls
 * that are DELIBERATELY INERT for now: PANEL_BTN_LOCAL_TOGGLE flips the
 * caption and sends nothing. The actuation hardware isn't wired yet; the
 * control surface is here so the layout is settled when it is.
 *
 * Tank instances are bus-confirmed: 0 = fresh, 1 = black, 2 = grey. The
 * GREY/BLACK labels are load-bearing — ui.c finds them by name to drive the
 * status bar's "Grey-Black OK/Warn/FULL" readout.
 */
static const panel_btn_def_t PANEL_BUTTONS_3[] = {
    { .label = "FRESH", .type = PANEL_BTN_TANK_LEVEL, .instances = {0}, .instance_count = 1 },
    { .label = "GREY", .type = PANEL_BTN_TANK_LEVEL, .instances = {2}, .instance_count = 1 },
    { .label = "BLACK", .type = PANEL_BTN_TANK_LEVEL, .instances = {1}, .instance_count = 1 },
    { .label = "GREY CLOSED", .type = PANEL_BTN_LOCAL_TOGGLE, .instances = {0}, .instance_count = 0, .label_alt = "GREY OPEN" },
    { .label = "BLACK CLOSED", .type = PANEL_BTN_LOCAL_TOGGLE, .instances = {0}, .instance_count = 0, .label_alt = "BLACK OPEN" },
    { .label = "GRAVITY", .type = PANEL_BTN_LOCAL_TOGGLE, .instances = {0}, .instance_count = 0, .label_alt = "MACERATOR" },
};

#define PANEL_BUTTON_COUNT_3 (sizeof(PANEL_BUTTONS_3) / sizeof(PANEL_BUTTONS_3[0]))
