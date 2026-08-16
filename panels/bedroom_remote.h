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
