/*
 * Panel: ent_center — replaces Entegra SW4-E1 (p/n 0291136 / 75571).
 * Build: idf.py -DPANEL=ent_center build
 *
 * 2-column x 4-row grid, reading order (row-major). Bottom-right cell is
 * empty as of GitHub issue #3 (PANEL LIGHTS button removed, backlight is
 * now automatic) until issue #4's screen-switch button fills it:
 *   CENTER CEILING | HALL/MIDSHIP
 *   SIDE CEILING   | SINK/COUNTER
 *   ODS SLIDE      | SCONCE/DINETTE
 *   ACCENT         |
 */
#pragma once

#include "lvgl.h"
#include "panel_def.h"

#define PANEL_NAME  "ENT CENTER"
#define PANEL_INDEX 1

static const panel_btn_def_t PANEL_BUTTONS[] = {
    { .label = "CENTER CEILING", .type = PANEL_BTN_DIMMER, .instances = {25}, .instance_count = 1 },
    { .label = "HALL/MIDSHIP", .type = PANEL_BTN_DIMMER, .instances = {35}, .instance_count = 1 },
    { .label = "SIDE CEILING", .type = PANEL_BTN_DIMMER, .instances = {30, 31}, .instance_count = 2 },
    { .label = "SINK/COUNTER", .type = PANEL_BTN_DIMMER, .instances = {34}, .instance_count = 1 },
    { .label = "ODS SLIDE", .type = PANEL_BTN_DIMMER, .instances = {32}, .instance_count = 1 },
    { .label = "SCONCE/DINETTE", .type = PANEL_BTN_DIMMER, .instances = {33}, .instance_count = 1 },
    { .label = "ACCENT", .type = PANEL_BTN_DIMMER, .instances = {26, 27}, .instance_count = 2 },
};

#define PANEL_BUTTON_COUNT (sizeof(PANEL_BUTTONS) / sizeof(PANEL_BUTTONS[0]))
