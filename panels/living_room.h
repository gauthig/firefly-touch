/*
 * Panel: living_room (on-screen name "MID COACH") — replaces Entegra SW2-E8
 * (p/n 0291135 / 75570).
 * Build: idf.py -DPANEL=living_room build
 *
 * 2-column x 4-row grid, reading order (row-major). Bottom-right cell is
 * empty as of GitHub issue #3 (PANEL LIGHTS button removed, backlight is
 * now automatic) until issue #4's screen-switch button fills it:
 *   CENTER CEILING     | SIDE CEILING
 *   SOFA SCONCE/SLIDE  | SINK/COUNTER
 *   DINETTE            | ACCENT
 *   MIDSHIP            |
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

static const panel_btn_def_t PANEL_BUTTONS[] = {
    { "CENTER CEILING",  PANEL_BTN_DIMMER, {25},     1 },
    { "MIDSHIP",         PANEL_BTN_DIMMER, {35},     1 },
    { "SIDE CEILING",    PANEL_BTN_DIMMER, {30, 31}, 2 },
    { "SINK/COUNTER",    PANEL_BTN_DIMMER, {34},     1 },
    { "SOFA SCONCE/SLIDE", PANEL_BTN_DIMMER, {32},   1 },
    { "DINETTE",         PANEL_BTN_DIMMER, {33},     1 },
    { "ACCENT",          PANEL_BTN_DIMMER, {26, 27}, 2 },
};

#define PANEL_BUTTON_COUNT (sizeof(PANEL_BUTTONS) / sizeof(PANEL_BUTTONS[0]))
