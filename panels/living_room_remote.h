/*
 * Panel: living_room_remote — no CAN wiring. Relays commands to, and
 * mirrors status from, panels/living_room.h (PANEL_IS_BRIDGE=1) over
 * ESP-NOW. Bridge forwards whatever instance it's given onto the real bus
 * regardless of living_room's own button list, so this panel's instances
 * don't need to match living_room.h's — see docs/instance_map.yaml for the
 * full RV-C instance map this was chosen from.
 * Build: idf.py -DPANEL=living_room_remote build
 *
 * Before flashing: set CONFIG_FIREFLY_ESPNOW_PEER_MAC to living_room's MAC
 * (and living_room's build to this panel's MAC), and change
 * CONFIG_FIREFLY_ESPNOW_PMK/LMK from the placeholder defaults — see
 * main/Kconfig.projbuild.
 */
#pragma once

#include "lvgl.h"
#include "panel_def.h"

#define PANEL_NAME  "LR REMOTE"
#define PANEL_INDEX 2
#define PANEL_HAS_CAN 0
#define PANEL_HAS_OTA 1

static const panel_btn_def_t PANEL_BUTTONS[] = {
    { "BEDROOM CEILING", PANEL_BTN_DIMMER, {17},     1 },
    { "BED O/H",         PANEL_BTN_DIMMER, {18},     1 },
    { "CENTER CEILING",  PANEL_BTN_DIMMER, {25},     1 },
    { "BATHROOM",        PANEL_BTN_DIMMER, {13},     1 },
    { "MIDSHIP",         PANEL_BTN_DIMMER, {35},     1 },
    { "COURTESY",        PANEL_BTN_DIMMER, {21},     1 },
    { "MOTION",          PANEL_BTN_SWITCH, {46},     1 },
    /* Bottom-right cell empty as of GitHub issue #3 (PANEL LIGHTS button
     * removed, backlight is now automatic) until issue #4's screen-switch
     * button fills it. */
};

#define PANEL_BUTTON_COUNT (sizeof(PANEL_BUTTONS) / sizeof(PANEL_BUTTONS[0]))
