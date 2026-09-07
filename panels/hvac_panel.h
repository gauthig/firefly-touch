/*
 * Panel: hvac_panel (on-screen name "HVAC") — the spare Waveshare
 * ESP32-S3-Touch-LCD-4.3B (the board hvac_capture was flashed to, COM23),
 * turned into a standalone auxiliary panel.
 *
 * Build:
 *   idf.py -B build_hvac_panel -DPANEL=hvac_panel \
 *          -DSDKCONFIG=build_hvac_panel/sdkconfig build
 *
 * Set CONFIG_FIREFLY_EASYTOUCH_PASSWORD in build_hvac_panel/sdkconfig by
 * hand before flashing (the Micro-Air account password — see
 * docs/EASYTOUCH-THERMOSTAT.md). Never in sdkconfig.defaults.
 *
 * ROLE: no CAN wiring. It is the coach's THERMOSTAT BRIDGE
 * (PANEL_HVAC_BRIDGE): it holds the one BLE link to the EasyTouch RV
 * thermostat (PANEL_HAS_EASYTOUCH, components/easytouch), controls it from
 * its own screen, broadcasts each zone over ESP-NOW (ESPNOW_TELEM_HVAC) so
 * other panels can display it, and accepts ESPNOW_FRAME_HVAC_CMD from those
 * panels (FIREFLY_ESPNOW_RX_PEER_MAC_1/_2 = bedroom_remote, main_cabinet),
 * feeding them to the local client -- the same pattern mid_coach uses to
 * bridge light commands onto CAN. It also shows Power / Batteries / Tanks
 * from the broadcasts the proxy and the mid_coach bridge already put on the
 * channel. It sends no unicast itself.
 *
 * INSTALLED in the stool room, where the Firefly G6 unit is.
 *
 * ⚠️ Naming: the registry convention is "ESP-NOW panel ⇒ id ends _remote".
 * This one is deliberately `hvac_panel` (it is thermostat-first and not a
 * command relay); tools/check_panels.py does not enforce the suffix.
 *
 * SCREENS: screen 0 is a launcher menu; the four detail screens sit beneath
 * it and each has a BACK button. idle-off returns to the menu
 * (PANEL_DEFAULT_SCREEN 0).
 *
 *   0  MENU        four buttons -> the screens below
 *   1  THERMOSTAT  PANEL_BTN_THERMOSTAT (3 zone cards) + BACK
 *   2  POWER       PANEL_BTN_SHORE_POWER (Hughes Power Watchdog) + BACK
 *   3  BATTERIES   PANEL_BTN_BATTERY_SUMMARY (combined bank) + BACK
 *   4  TANKS       FRESH / GREY / BLACK SeeLevel gauges + BACK
 */
#pragma once

#include "lvgl.h"
#include "panel_def.h"

#define PANEL_NAME  "HVAC"
#define PANEL_INDEX 4

/* Per-panel version minor; the major is global in main/firefly_version.h.
 * Bump this ONLY for a fix that touches just this panel, and update
 * docs/FLASHING.md's status table in the same commit. Panels are
 * expected to sit on different minors — that is the design, not drift. */
#define PANEL_VERSION_MINOR 0

#define PANEL_HAS_CAN 0
#define PANEL_HAS_EASYTOUCH 1   /* implies PANEL_HAS_THERMOSTAT */
#define PANEL_HVAC_BRIDGE 1     /* accepts ESPNOW_FRAME_HVAC_CMD from other panels */

#define PANEL_HAS_SCREEN_2 1
#define PANEL_HAS_SCREEN_3 1
#define PANEL_HAS_SCREEN_4 1
#define PANEL_HAS_SCREEN_5 1
#define PANEL_DEFAULT_SCREEN 0

/*
 * Screen 0 — MENU. Four PANEL_BTN_SCREEN_SWITCH buttons, each naming its
 * target screen in instances[0] (0 would be this menu itself). Laid out by
 * build_button_grid() as a 2x2 grid.
 */
static const panel_btn_def_t PANEL_BUTTONS[] = {
    { .label = "THERMOSTAT", .type = PANEL_BTN_SCREEN_SWITCH, .instances = {1}, .instance_count = 1 },
    { .label = "POWER",      .type = PANEL_BTN_SCREEN_SWITCH, .instances = {2}, .instance_count = 1 },
    { .label = "BATTERIES",  .type = PANEL_BTN_SCREEN_SWITCH, .instances = {3}, .instance_count = 1 },
    { .label = "TANKS",      .type = PANEL_BTN_SCREEN_SWITCH, .instances = {4}, .instance_count = 1 },
};

#define PANEL_BUTTON_COUNT (sizeof(PANEL_BUTTONS) / sizeof(PANEL_BUTTONS[0]))

/* Screen 1 — THERMOSTAT. Fed by the on-panel EasyTouch BLE client; takes no
 * instances (not an RV-C node). */
static const panel_btn_def_t PANEL_BUTTONS_2[] = {
    { .label = "THERMOSTAT", .type = PANEL_BTN_THERMOSTAT, .instances = {0}, .instance_count = 0 },
    { .label = "BACK", .type = PANEL_BTN_SCREEN_SWITCH, .instances = {0}, .instance_count = 1 },
};

#define PANEL_BUTTON_COUNT_2 (sizeof(PANEL_BUTTONS_2) / sizeof(PANEL_BUTTONS_2[0]))

/* Screen 2 — POWER. Hughes Power Watchdog Line 1 / Line 2, from the proxy's
 * ESP-NOW broadcast. */
static const panel_btn_def_t PANEL_BUTTONS_3[] = {
    { .label = "SHORE", .type = PANEL_BTN_SHORE_POWER, .instances = {0}, .instance_count = 0 },
    { .label = "BACK", .type = PANEL_BTN_SCREEN_SWITCH, .instances = {0}, .instance_count = 1 },
};

#define PANEL_BUTTON_COUNT_3 (sizeof(PANEL_BUTTONS_3) / sizeof(PANEL_BUTTONS_3[0]))

/* Screen 3 — BATTERIES. Combined bank across the three parallel packs, from
 * the proxy's per-pack broadcasts run through jbd_bms_combine(). Tap the
 * readout for the per-pack detail popup. */
static const panel_btn_def_t PANEL_BUTTONS_4[] = {
    { .label = "BANK", .type = PANEL_BTN_BATTERY_SUMMARY, .instances = {0}, .instance_count = 0 },
    { .label = "BACK", .type = PANEL_BTN_SCREEN_SWITCH, .instances = {0}, .instance_count = 1 },
};

#define PANEL_BUTTON_COUNT_4 (sizeof(PANEL_BUTTONS_4) / sizeof(PANEL_BUTTONS_4[0]))

/*
 * Screen 4 — TANKS. The mid_coach bridge broadcasts the tanks it displays
 * (ESPNOW_TELEM_TANK); this panel re-enters them through ui_on_tank_status()
 * exactly like a CAN-fed panel. GREY / BLACK labels are load-bearing: ui.c
 * finds them to drive the status-bar "Grey-Black OK/Warn/FULL" readout and
 * the tank-critical backlight override. Instances bus-confirmed:
 * 0 = fresh, 1 = black, 2 = grey.
 */
static const panel_btn_def_t PANEL_BUTTONS_5[] = {
    { .label = "FRESH", .type = PANEL_BTN_TANK_LEVEL, .instances = {0}, .instance_count = 1 },
    { .label = "GREY",  .type = PANEL_BTN_TANK_LEVEL, .instances = {2}, .instance_count = 1 },
    { .label = "BLACK", .type = PANEL_BTN_TANK_LEVEL, .instances = {1}, .instance_count = 1 },
    { .label = "BACK", .type = PANEL_BTN_SCREEN_SWITCH, .instances = {0}, .instance_count = 1 },
};

#define PANEL_BUTTON_COUNT_5 (sizeof(PANEL_BUTTONS_5) / sizeof(PANEL_BUTTONS_5[0]))
