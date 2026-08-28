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
    { .label = "BEDROOM CEILING", .type = PANEL_BTN_DIMMER, .instances = {17}, .instance_count = 1 },
    { .label = "BED O/H", .type = PANEL_BTN_DIMMER, .instances = {18}, .instance_count = 1 },
    { .label = "CENTER CEILING", .type = PANEL_BTN_DIMMER, .instances = {25}, .instance_count = 1 },
    { .label = "BATHROOM", .type = PANEL_BTN_DIMMER, .instances = {13}, .instance_count = 1 },
    { .label = "MIDSHIP", .type = PANEL_BTN_DIMMER, .instances = {35}, .instance_count = 1 },
    { .label = "COURTESY", .type = PANEL_BTN_DIMMER, .instances = {21}, .instance_count = 1 },
    { .label = "MOTION", .type = PANEL_BTN_SWITCH, .instances = {46}, .instance_count = 1 },
    { .label = "", .type = PANEL_BTN_SPACER, .instances = {0}, .instance_count = 0 },
    { .label = "BATTERY", .type = PANEL_BTN_SCREEN_SWITCH, .instances = {1}, .instance_count = 1 },
    { .label = "SHORE POWER", .type = PANEL_BTN_SCREEN_SWITCH, .instances = {2}, .instance_count = 1 },
};

#define PANEL_BUTTON_COUNT (sizeof(PANEL_BUTTONS) / sizeof(PANEL_BUTTONS[0]))

/*
 * The three packs are wired in PARALLEL, so screen 2 is one combined bank
 * readout rather than three per-pack gauges — one voltage, one current, one
 * power, one SOC, one time-remaining, the way the coach's own Vatrer display
 * presents it. Tapping the readout reveals per-pack detail (MAC, SOC, volts,
 * amps, temp) for telling which physical pack is which. The summary takes no
 * instances: it aggregates every configured battery slot itself.
 *
 * The Renogy MPPT readout sits underneath it, on the same screen rather than
 * a screen of its own: solar is what is PUTTING energy into that bank, so
 * the two belong side by side — seeing 74 W coming in above a bank that says
 * "Fully Charged In 3h" is the whole story in one glance. build_screen2_row()
 * stacks them (two full-width readouts can't share a 480 px row) and gives
 * the bank whatever height the solar strip does not take.
 */
static const panel_btn_def_t PANEL_BUTTONS_2[] = {
    { .label = "BANK", .type = PANEL_BTN_BATTERY_SUMMARY, .instances = {0}, .instance_count = 0 },
    { .label = "SOLAR", .type = PANEL_BTN_SOLAR, .instances = {0}, .instance_count = 0 },
    { .label = "BACK", .type = PANEL_BTN_SCREEN_SWITCH, .instances = {0}, .instance_count = 1 },
};

#define PANEL_BUTTON_COUNT_2 (sizeof(PANEL_BUTTONS_2) / sizeof(PANEL_BUTTONS_2[0]))

/*
 * Screen 3: shore power (Hughes Power Watchdog), Line 1 / Line 2 volts,
 * amps, frequency and watts. The data arrives as an ESP-NOW telemetry
 * broadcast from the basement BLE proxy — this panel has no BLE link to the
 * Watchdog itself, and needs none.
 */
static const panel_btn_def_t PANEL_BUTTONS_3[] = {
    { .label = "SHORE", .type = PANEL_BTN_SHORE_POWER, .instances = {0}, .instance_count = 0 },
    { .label = "BACK", .type = PANEL_BTN_SCREEN_SWITCH, .instances = {0}, .instance_count = 1 },
};

#define PANEL_BUTTON_COUNT_3 (sizeof(PANEL_BUTTONS_3) / sizeof(PANEL_BUTTONS_3[0]))
