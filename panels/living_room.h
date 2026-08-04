/*
 * Panel: living_room — replaces Entegra SW2-E8 (p/n 0291135 / 75570).
 * Build: idf.py -DPANEL=living_room build
 *
 * 2-column x 4-row grid, reading order (row-major):
 *   CENTER CEILING | ENTRY CEILING
 *   SIDE CEILING   | DINETTE
 *   ODS SOFA SCONCE| MIDSHIP
 *   SECURITY P+H   | PANEL LIGHTS
 */
#pragma once

#include "lvgl.h"
#include "panel_def.h"

#define PANEL_NAME  "LIVING ROOM"
#define PANEL_INDEX 0

static const panel_btn_def_t PANEL_BUTTONS[] = {
    { "CENTER CEILING",  LV_SYMBOL_CHARGE,   PANEL_BTN_DIMMER, {25},     1 },
    { "ENTRY CEILING",   LV_SYMBOL_CHARGE,   PANEL_BTN_DIMMER, {24},     1 },
    { "SIDE CEILING",    LV_SYMBOL_CHARGE,   PANEL_BTN_DIMMER, {30, 31}, 2 },
    { "DINETTE",         LV_SYMBOL_CHARGE,   PANEL_BTN_DIMMER, {33},     1 },
    { "ODS SOFA SCONCE", LV_SYMBOL_CHARGE,   PANEL_BTN_DIMMER, {32},     1 },
    { "MIDSHIP",         LV_SYMBOL_CHARGE,   PANEL_BTN_DIMMER, {35},     1 },
    /* Factory legend "P45, H44, 45" = patio + hitch lights, driven together
     * as plain on/off here.
     * TODO: verify via sniffer — the P/H prefixes may indicate a different
     * load type or a scene rather than plain dimmer instances 44/45. */
    { "SECURITY P+H",    LV_SYMBOL_EYE_OPEN, PANEL_BTN_SWITCH, {44, 45}, 2 },
    /* Factory legend "PL1" — factory switch-panel backlight control, likely
     * NOT a DC_DIMMER instance. Placeholder: logs the press and cycles the
     * local LCD backlight. TODO: capture the factory panel's frames in
     * sniffer mode and implement whatever DGN it actually uses. */
    { "PANEL LIGHTS",    LV_SYMBOL_SETTINGS, PANEL_BTN_PANEL_LIGHTS, {0}, 0 },
};

#define PANEL_BUTTON_COUNT (sizeof(PANEL_BUTTONS) / sizeof(PANEL_BUTTONS[0]))
