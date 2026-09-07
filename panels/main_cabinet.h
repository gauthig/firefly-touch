/*
 * Panel: main_cabinet (on-screen name "MAIN CABINET") — the coach's main
 * cabinet touchscreen.
 * Build: idf.py -B build_main_cabinet -DPANEL=main_cabinet build
 *
 * FIRST PANEL ON A DIFFERENT BOARD: a Waveshare ESP32-S3-Touch-LCD-7B
 * (1024x600), not the 4.3B the other three use. The root CMakeLists.txt
 * maps PANEL -> BOARD, so nothing here selects it; see
 * components/board/board_lcd7b for why that mapping matters (CAN moves to
 * GPIO20/19, where the 4.3B has RS485, and it is muxed against native USB).
 * It was previously the non-B 800x480 7" (components/board/board_lcd7),
 * kept as an unused reference.
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
 *   SOLAR  -> screen 3   Renogy MPPT readout
 *   TANKS  -> screen 2   fresh/grey/black gauges (display only)
 *   LIGHTS -> screen 0   the button grid
 *
 * SOLAR is its own section rather than a third card on POWER: it started
 * that way when this was the 800x480 non-B panel and the simulator showed
 * POWER already full minus the rail. On the 1024x600 7B the battery bank
 * and shore power now sit comfortably side by side, but SOLAR stays its
 * own section — a rail panel pays nothing for another one, since the rail
 * navigates by target screen, not by position.
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

/* Per-panel version minor; the major is global in main/firefly_version.h.
 * Bump this ONLY for a fix that touches just this panel, and update
 * docs/FLASHING.md's status table in the same commit. Panels are
 * expected to sit on different minors — that is the design, not drift. */
#define PANEL_VERSION_MINOR 0

/* Hardwired to the RV-C bus (source address 0x83 = 0x80 + 3). */

/* Battery and shore-power readings exist only as ESP-NOW broadcasts from
 * the basement BLE proxy, so this panel listens on the broadcast channel
 * even though it has its own bus. Receive-only: no peer, no keys. */
#define PANEL_WANTS_TELEMETRY 1

#define PANEL_HAS_SCREEN_2 1
#define PANEL_HAS_SCREEN_3 1
#define PANEL_HAS_SCREEN_4 1
#define PANEL_HAS_SCREEN_5 1
#define PANEL_HAS_NAV_RAIL 1

/*
 * Thermostat: no BLE link here -- hvac_panel (in the stool room) holds it
 * and broadcasts each zone. This panel shows a CLIMATE rail section and
 * sends changes back to hvac_panel over ESP-NOW. PANEL_WANTS_HVAC_CONTROL
 * needs FIREFLY_ESPNOW_HVAC_PEER_MAC = hvac_panel's MAC, and hvac_panel
 * needs this panel's MAC in its FIREFLY_ESPNOW_RX_PEER_MAC_1/_2.
 */
#define PANEL_HAS_THERMOSTAT 1
#define PANEL_WANTS_HVAC_CONTROL 1

/* Carries the MASTER and ALL LIGHTS buttons, so panel_config.h can check
 * both against PANEL_HAS_CAN. */
#define PANEL_HAS_LIGHT_MASTER 1
#define PANEL_HAS_LIGHT_SWEEP  1
#define PANEL_DEFAULT_SCREEN 1   /* boot into POWER */
#define PANEL_GRID_COLS 3        /* 1024 px minus the rail: 3 wide cells read better
                                  * than 4 narrow ones for the long load names */

/* Waveshare ESP32-S3-Touch-LCD-7B, run in its native landscape orientation
 * (no rotation), so logical == physical. Overrides panel_config.h's
 * nav-rail default of 800x480. Drives the simulator window size; the
 * firmware reads BOARD_LCD_H/V_RES from board_lcd7b.h. */
#define PANEL_LOGICAL_W 1024
#define PANEL_LOGICAL_H 600

/*
 * The rail. Ordinary PANEL_BTN_SCREEN_SWITCH entries — instances[0] names
 * the target screen, exactly as on the other panels' nav buttons.
 */
static const panel_btn_def_t PANEL_NAV_RAIL[] = {
    { .label = "CLIMATE", .type = PANEL_BTN_SCREEN_SWITCH, .instances = {4}, .instance_count = 1 },
    { .label = "POWER", .type = PANEL_BTN_SCREEN_SWITCH, .instances = {1}, .instance_count = 1 },
    { .label = "SOLAR", .type = PANEL_BTN_SCREEN_SWITCH, .instances = {3}, .instance_count = 1 },
    { .label = "TANKS", .type = PANEL_BTN_SCREEN_SWITCH, .instances = {2}, .instance_count = 1 },
    { .label = "LIGHTS", .type = PANEL_BTN_SCREEN_SWITCH, .instances = {0}, .instance_count = 1 },
};

#define PANEL_NAV_RAIL_COUNT (sizeof(PANEL_NAV_RAIL) / sizeof(PANEL_NAV_RAIL[0]))

/*
 * Screen 0 — LIGHTS. Three columns x four rows, full: MASTER leads, ALL
 * LIGHTS fills the bottom-right cell.
 *
 * MASTER (PANEL_BTN_LIGHT_MASTER) replays the coach's factory rocker — six
 * group frames at once, restoring each load's remembered level. ALL LIGHTS
 * (PANEL_BTN_LIGHT_SWEEP) is a plain sequential all-on/all-off: ui.c walks
 * every DIMMER/SWITCH instance below and sends it an explicit ON/OFF 100 ms
 * apart. The two are deliberately both present — one is the factory
 * behaviour, the other a literal "everything".
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
    { .label = "ALL LIGHTS", .type = PANEL_BTN_LIGHT_SWEEP, .instances = {0}, .instance_count = 0 },
};

#define PANEL_BUTTON_COUNT (sizeof(PANEL_BUTTONS) / sizeof(PANEL_BUTTONS[0]))

/*
 * MASTER needs no scene list any more. It used to declare one because the
 * factory rocker's frames were unknown; they were captured 2026-08-28, so
 * ui.c now replays exactly what the rocker sends — six group-addressed
 * frames, MEMORY_OFF for off and "restore remembered level" for on. The
 * groups live in main/panel_config.h (PANEL_MASTER_GROUPS) because they are
 * a property of the coach, not of this panel.
 */

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
 * Screen 2 — TANKS. The three SeeLevel gauges, display-only.
 *
 * The dump-valve / gravity-macerator toggle row that mid_coach carries was
 * removed here (issue #68): valve actuation will be exposed on mid_coach
 * only for now, and with no action buttons this screen takes build_
 * content_pane()'s byte-identical no-action-row path.
 *
 * Tank instances are bus-confirmed: 0 = fresh, 1 = black, 2 = grey. The
 * GREY/BLACK labels are load-bearing — ui.c finds them by name to drive the
 * status bar's "Grey-Black OK/Warn/FULL" readout.
 */
static const panel_btn_def_t PANEL_BUTTONS_3[] = {
    { .label = "FRESH", .type = PANEL_BTN_TANK_LEVEL, .instances = {0}, .instance_count = 1 },
    { .label = "GREY", .type = PANEL_BTN_TANK_LEVEL, .instances = {2}, .instance_count = 1 },
    { .label = "BLACK", .type = PANEL_BTN_TANK_LEVEL, .instances = {1}, .instance_count = 1 },
};

#define PANEL_BUTTON_COUNT_3 (sizeof(PANEL_BUTTONS_3) / sizeof(PANEL_BUTTONS_3[0]))

/*
 * Screen 3 — SOLAR. The Renogy MPPT charge controller: PV watts/volts/amps,
 * battery volts, and controller/battery temperature in F, with the charging
 * state (mppt / boost / float) in the header.
 *
 * Arrives as an ESP-NOW broadcast from the basement proxy, which holds the
 * BLE link to the controller's BT-2 module — this panel has no link to it
 * and needs none, exactly like the battery bank and shore power above.
 * Takes no instances: the controller is a BLE peer, not an RV-C node.
 */
static const panel_btn_def_t PANEL_BUTTONS_4[] = {
    { .label = "SOLAR", .type = PANEL_BTN_SOLAR, .instances = {0}, .instance_count = 0 },
};

#define PANEL_BUTTON_COUNT_4 (sizeof(PANEL_BUTTONS_4) / sizeof(PANEL_BUTTONS_4[0]))

/*
 * Screen index 4 (PANEL_BUTTONS_5, the 5th screen) — CLIMATE. The three
 * EasyTouch zones, fed by hvac_panel's ESPNOW_TELEM_HVAC broadcast;
 * mode/setpoint changes go back to hvac_panel over ESP-NOW. No BACK -- the
 * persistent rail is the navigation. (Rail entry targets {4}: PANEL_BUTTONS_N
 * lays out screen N-1.)
 */
static const panel_btn_def_t PANEL_BUTTONS_5[] = {
    { .label = "THERMOSTAT", .type = PANEL_BTN_THERMOSTAT, .instances = {0}, .instance_count = 0 },
};

#define PANEL_BUTTON_COUNT_5 (sizeof(PANEL_BUTTONS_5) / sizeof(PANEL_BUTTONS_5[0]))
