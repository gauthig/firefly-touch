/*
 * Panel: ent_center — replaces Entegra SW4-E1 (p/n 0291136 / 75571).
 * Build: idf.py -DPANEL=ent_center build
 *
 * 2-column x 4-row grid, reading order (row-major):
 *   CENTER CEILING | HALL/MIDSHIP
 *   SIDE CEILING   | SINK/COUNTER
 *   ODS SLIDE      | SCONCE/DINETTE
 *   ACCENT         | PANEL LIGHTS
 */
#pragma once

#include "lvgl.h"
#include "panel_def.h"

#define PANEL_NAME  "ENT CENTER"
#define PANEL_INDEX 1

static const panel_btn_def_t PANEL_BUTTONS[] = {
    { "CENTER CEILING",  LV_SYMBOL_CHARGE,   PANEL_BTN_DIMMER, {25},     1 },
    { "HALL/MIDSHIP",    LV_SYMBOL_CHARGE,   PANEL_BTN_DIMMER, {35},     1 },
    { "SIDE CEILING",    LV_SYMBOL_CHARGE,   PANEL_BTN_DIMMER, {30, 31}, 2 },
    { "SINK/COUNTER",    LV_SYMBOL_CHARGE,   PANEL_BTN_DIMMER, {34},     1 },
    { "ODS SLIDE",       LV_SYMBOL_CHARGE,   PANEL_BTN_DIMMER, {32},     1 },
    { "SCONCE/DINETTE",  LV_SYMBOL_CHARGE,   PANEL_BTN_DIMMER, {33},     1 },
    { "ACCENT",          LV_SYMBOL_CHARGE,   PANEL_BTN_DIMMER, {26, 27}, 2 },
    /* Factory legend "PL1" — see living_room.h PANEL LIGHTS note. */
    { "PANEL LIGHTS",    LV_SYMBOL_SETTINGS, PANEL_BTN_PANEL_LIGHTS, {0}, 0 },
};

#define PANEL_BUTTON_COUNT (sizeof(PANEL_BUTTONS) / sizeof(PANEL_BUTTONS[0]))
