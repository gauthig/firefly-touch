/*
 * Panel: mid_coach (on-screen name "MID COACH") — replaces Entegra SW2-E8
 * (p/n 0291135 / 75570).
 * Build: idf.py -DPANEL=mid_coach build
 *
 * Screen 1 — 2-column x 4-row grid, reading order (row-major). Bottom-right
 * is the screen-switch button (GitHub issue #4) that flips to screen 2:
 *   CENTER CEILING     | SIDE CEILING
 *   SOFA SCONCE/SLIDE  | SINK/COUNTER
 *   DINETTE            | ACCENT
 *   MIDSHIP            | TANK LEVELS
 *
 * Screen 2 — tank levels (SeeLevel II 709-RVC, GitHub issue #5), reworked
 * to an animated wave-gauge row + small pinned BACK button (issues #9-#11):
 * FRESH/GREY/BLACK lay out as a centered row of gauges (main/ui/ui.c's
 * build_screen2_row()), BACK as a small button pinned bottom-center. No
 * manual grid/spacer positioning needed for this layout, unlike screen 1.
 * The status bar also reads the GREY/BLACK buttons here by label to drive
 * the header's "Grey-Black OK/Warn/FULL" readout (issue #9).
 *
 * Instance numbers (0=fresh, 1=black, 2=gray) per docs/instance_map.yaml ->
 * tank_sensors -- still unverified against this coach's actual bus traffic,
 * confirm via sniffer (notably the black/grey ordering) before trusting.
 */
#pragma once

#include "lvgl.h"
#include "panel_def.h"

#define PANEL_NAME  "MID COACH"
#define PANEL_INDEX 0

/* This panel is the ESP-NOW bridge/router between the RV-C CAN bus and all
 * remote panels (any panel whose ID ends in _remote, e.g.
 * panels/bedroom_remote.h). It relays remote commands onto this bus and
 * mirrors real status back to them. See main/panel_config.h and
 * components/espnow_link. */
#define PANEL_IS_BRIDGE 1

#define PANEL_HAS_SCREEN_2 1

static const panel_btn_def_t PANEL_BUTTONS[] = {
    { .label = "CENTER CEILING", .type = PANEL_BTN_DIMMER, .instances = {25}, .instance_count = 1 },
    { .label = "MIDSHIP", .type = PANEL_BTN_DIMMER, .instances = {35}, .instance_count = 1 },
    { .label = "SIDE CEILING", .type = PANEL_BTN_DIMMER, .instances = {30, 31}, .instance_count = 2 },
    { .label = "SINK/COUNTER", .type = PANEL_BTN_DIMMER, .instances = {34}, .instance_count = 1 },
    { .label = "SOFA SCONCE/SLIDE", .type = PANEL_BTN_DIMMER, .instances = {32}, .instance_count = 1 },
    { .label = "DINETTE", .type = PANEL_BTN_DIMMER, .instances = {33}, .instance_count = 1 },
    { .label = "ACCENT", .type = PANEL_BTN_DIMMER, .instances = {26, 27}, .instance_count = 2 },
    { .label = "TANK LEVELS", .type = PANEL_BTN_SCREEN_SWITCH, .instances = {0}, .instance_count = 0 },
};

#define PANEL_BUTTON_COUNT (sizeof(PANEL_BUTTONS) / sizeof(PANEL_BUTTONS[0]))

static const panel_btn_def_t PANEL_BUTTONS_2[] = {
    { .label = "FRESH", .type = PANEL_BTN_TANK_LEVEL, .instances = {0}, .instance_count = 1 },
    { .label = "GREY", .type = PANEL_BTN_TANK_LEVEL, .instances = {2}, .instance_count = 1 },
    { .label = "BLACK", .type = PANEL_BTN_TANK_LEVEL, .instances = {1}, .instance_count = 1 },
    { .label = "BACK", .type = PANEL_BTN_SCREEN_SWITCH, .instances = {0}, .instance_count = 0 },
};

#define PANEL_BUTTON_COUNT_2 (sizeof(PANEL_BUTTONS_2) / sizeof(PANEL_BUTTONS_2[0]))
