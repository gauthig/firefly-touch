/*
 * Panel: living_room (on-screen name "MID COACH") — replaces Entegra SW2-E8
 * (p/n 0291135 / 75570).
 * Build: idf.py -DPANEL=living_room build
 *
 * Screen 1 — 2-column x 4-row grid, reading order (row-major). Bottom-right
 * is the screen-switch button (GitHub issue #4) that flips to screen 2:
 *   CENTER CEILING     | SIDE CEILING
 *   SOFA SCONCE/SLIDE  | SINK/COUNTER
 *   DINETTE            | ACCENT
 *   MIDSHIP            | TANK LEVELS
 *
 * Screen 2 — tank levels (SeeLevel II 709-RVC, GitHub issue #5). BACK sits
 * at the bottom-right (same convention: the screen-switch button always
 * lives in the same cell on every screen it appears on) via PANEL_BTN_SPACER
 * filler cells:
 *   FRESH  | GREY
 *   BLACK  |
 *          |
 *          | BACK
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

/* Also the ESP-NOW bridge for panels/living_room_remote.h: relays that
 * panel's commands onto this bus and mirrors real status back to it. See
 * main/panel_config.h and components/espnow_link. */
#define PANEL_IS_BRIDGE 1

#define PANEL_HAS_SCREEN_2 1

static const panel_btn_def_t PANEL_BUTTONS[] = {
    { "CENTER CEILING",  PANEL_BTN_DIMMER, {25},     1 },
    { "MIDSHIP",         PANEL_BTN_DIMMER, {35},     1 },
    { "SIDE CEILING",    PANEL_BTN_DIMMER, {30, 31}, 2 },
    { "SINK/COUNTER",    PANEL_BTN_DIMMER, {34},     1 },
    { "SOFA SCONCE/SLIDE", PANEL_BTN_DIMMER, {32},   1 },
    { "DINETTE",         PANEL_BTN_DIMMER, {33},     1 },
    { "ACCENT",          PANEL_BTN_DIMMER, {26, 27}, 2 },
    { "TANK LEVELS",     PANEL_BTN_SCREEN_SWITCH, {0}, 0 },
};

#define PANEL_BUTTON_COUNT (sizeof(PANEL_BUTTONS) / sizeof(PANEL_BUTTONS[0]))

static const panel_btn_def_t PANEL_BUTTONS_2[] = {
    { "FRESH", PANEL_BTN_TANK_LEVEL,   {0}, 1 },
    { "GREY",  PANEL_BTN_TANK_LEVEL,   {2}, 1 },
    { "BLACK", PANEL_BTN_TANK_LEVEL,   {1}, 1 },
    { "",      PANEL_BTN_SPACER,       {0}, 0 },
    { "",      PANEL_BTN_SPACER,       {0}, 0 },
    { "",      PANEL_BTN_SPACER,       {0}, 0 },
    { "",      PANEL_BTN_SPACER,       {0}, 0 },
    { "BACK",  PANEL_BTN_SCREEN_SWITCH, {0}, 0 },
};

#define PANEL_BUTTON_COUNT_2 (sizeof(PANEL_BUTTONS_2) / sizeof(PANEL_BUTTONS_2[0]))
