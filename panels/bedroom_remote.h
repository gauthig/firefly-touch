/*
 * Panel: bedroom_remote — an ESP-NOW remote (no RV-C CAN wiring). Relays
 * commands to, and mirrors status from, panels/mid_coach.h
 * (PANEL_IS_BRIDGE=1) over ESP-NOW. Bridge forwards whatever instance it's
 * given onto the real bus regardless of mid_coach's own button list, so
 * this panel's instances don't need to match mid_coach.h's — see
 * docs/instance_map.yaml for the full RV-C instance map this was chosen
 * from.
 * Build: idf.py -DPANEL=bedroom_remote build
 *
 * Before flashing: set CONFIG_FIREFLY_ESPNOW_PEER_MAC to mid_coach's MAC
 * (and mid_coach's build to this panel's MAC), and change
 * CONFIG_FIREFLY_ESPNOW_PMK/LMK from the placeholder defaults — see
 * main/Kconfig.projbuild.
 *
 * This panel does NOT connect to the batteries. The basement proxy
 * (proxy/) holds all three BLE links and broadcasts the readings; the bank
 * screen below just displays what arrives. CONFIG_FIREFLY_BATTERY_1_MAC/
 * _2_MAC/_3_MAC are still worth setting to the same three MACs the proxy
 * uses, but here they are display labels for the per-pack detail popup
 * only — nothing on this panel dials them.
 *
 * Naming convention: any panel whose ID ends in _remote is an ESP-NOW
 * device (no RV-C CAN wiring) that reports to the Mid Coach bridge. Panels
 * without _remote in the ID are hardwired to the RV-C CAN bus.
 */
#pragma once

#include "lvgl.h"
#include "panel_def.h"

#define PANEL_NAME  "BED REMOTE"
#define PANEL_INDEX 2
#define PANEL_HAS_CAN 0
#define PANEL_HAS_SCREEN_2 1
#define PANEL_HAS_SCREEN_3 1

/*
 * Screen-switch buttons name their target screen in instances[0]:
 * 0 = this button grid, 1 = battery bank, 2 = shore power. The trailing
 * spacer keeps the two nav buttons together on the bottom row instead of
 * leaving one stranded beside MOTION.
 */
static const panel_btn_def_t PANEL_BUTTONS[] = {
    { "BEDROOM CEILING", PANEL_BTN_DIMMER, {17},     1 },
    { "BED O/H",         PANEL_BTN_DIMMER, {18},     1 },
    { "CENTER CEILING",  PANEL_BTN_DIMMER, {25},     1 },
    { "BATHROOM",        PANEL_BTN_DIMMER, {13},     1 },
    { "MIDSHIP",         PANEL_BTN_DIMMER, {35},     1 },
    { "COURTESY",        PANEL_BTN_DIMMER, {21},     1 },
    { "MOTION",          PANEL_BTN_SWITCH, {46},     1 },
    { "",                PANEL_BTN_SPACER, {0},      0 },
    { "BATTERY",         PANEL_BTN_SCREEN_SWITCH, {1}, 1 },
    { "SHORE POWER",     PANEL_BTN_SCREEN_SWITCH, {2}, 1 },
};

#define PANEL_BUTTON_COUNT (sizeof(PANEL_BUTTONS) / sizeof(PANEL_BUTTONS[0]))

/*
 * The three packs are wired in PARALLEL, so screen 2 is one combined bank
 * readout rather than three per-pack gauges — one voltage, one current, one
 * power, one SOC, one time-remaining, the way the coach's own Vatrer display
 * presents it. Tapping the readout reveals per-pack detail (MAC, SOC, volts,
 * amps, temp) for telling which physical pack is which. The summary takes no
 * instances: it aggregates every configured battery slot itself.
 */
static const panel_btn_def_t PANEL_BUTTONS_2[] = {
    { "BANK", PANEL_BTN_BATTERY_SUMMARY, {0}, 0 },
    { "BACK", PANEL_BTN_SCREEN_SWITCH,   {0}, 1 },
};

#define PANEL_BUTTON_COUNT_2 (sizeof(PANEL_BUTTONS_2) / sizeof(PANEL_BUTTONS_2[0]))

/*
 * Screen 3: shore power (Hughes Power Watchdog), Line 1 / Line 2 volts,
 * amps, frequency and watts. The data arrives as an ESP-NOW telemetry
 * broadcast from the basement BLE proxy — this panel has no BLE link to the
 * Watchdog itself, and needs none.
 */
static const panel_btn_def_t PANEL_BUTTONS_3[] = {
    { "SHORE", PANEL_BTN_SHORE_POWER,   {0}, 0 },
    { "BACK",  PANEL_BTN_SCREEN_SWITCH, {0}, 1 },
};

#define PANEL_BUTTON_COUNT_3 (sizeof(PANEL_BUTTONS_3) / sizeof(PANEL_BUTTONS_3[0]))
