/*
 * ============================ PANEL TEMPLATE ============================
 *
 * Copy this file to panels/<your_panel>.h and edit. Do NOT build this file
 * directly — the #error below fires until you remove it.
 *
 *   Copy-Item panels\TEMPLATE.h panels\galley.h
 *
 * ------------------------------------------------------------------------
 * ⚠️  BEFORE YOU EDIT: PANEL_INDEX MUST BE UNIQUE
 * ------------------------------------------------------------------------
 * The RV-C source address is derived as 0x80 + PANEL_INDEX. Two panels with
 * the same index transmit from the same address on a shared CAN bus, which
 * causes arbitration faults that look like random dropped or corrupted
 * frames — not an obvious failure.
 *
 * Ask yourself which case you are in:
 *
 *   A. UPDATING AN EXISTING PANEL (mid_coach, ent_center, ...)
 *      -> You do not need this template. Edit that panel's header directly
 *         and KEEP its existing PANEL_INDEX.
 *
 *   B. ADDING A NEW PANEL
 *      -> Take the "Next free index" from panels/REGISTRY.md, use it below,
 *         and add a row to that file in the same change.
 *
 * Then verify:  python tools/check_panels.py
 * (CI runs the same check and fails on duplicates or a missing registry row.)
 * ========================================================================
 */
#error "TEMPLATE.h is not a buildable panel — copy it to panels/<name>.h, fill it in, and delete this #error line."

#pragma once

#include "lvgl.h"
#include "panel_def.h"

/* Shown in the status bar and the boot log. Keep it short — it shares the
 * status bar with the CAN-health dot. */
#define PANEL_NAME  "PANEL NAME"

/* Unique. See the warning above and panels/REGISTRY.md. */
#define PANEL_INDEX 99

/*
 * Naming convention: a panel whose <name> ends in `_remote` is an ESP-NOW
 * device (no RV-C CAN wiring) that reports to the Mid Coach bridge. A panel
 * without `_remote` in its name is hardwired to the RV-C CAN bus. mid_coach
 * is the ESP-NOW router/bridge between the RV-C bus and all remotes.
 *
 * Set to 0 for a panel with no CAN wiring at all — it relays
 * commands/status to/from a PANEL_IS_BRIDGE=1 panel over ESP-NOW instead.
 * See main/panel_config.h and components/espnow_link. Leave at 1 (the
 * default in panel_config.h, so this line can be omitted) for a normal
 * CAN-connected panel.
 */
#define PANEL_HAS_CAN 1

/*
 * Buttons in reading order for the 2-column x 4-row grid:
 *
 *   [0] [1]
 *   [2] [3]
 *   [4] [5]
 *   [6] [7]
 *
 * Fields: { label, type, { instances... }, instance_count }
 *
 * Types:
 *   PANEL_BTN_DIMMER  tap = toggle, press-and-hold = ramp, brightness bar
 *   PANEL_BTN_SWITCH  tap = on/off only, no ramp, no bar
 *
 * Backlight is automatic (dim/off on idle, wake on touch) — there is no
 * manual panel-lights button anymore. A future screen-switch button type
 * (GitHub issue #4) may take the 8th slot below; leave it empty until then
 * unless your panel only needs 7 buttons.
 *
 * Multi-instance buttons drive every listed instance together and show ON if
 * any of them reports on. Instance numbers come from the factory switch
 * legend and MUST be confirmed with a sniffer build — see CLAUDE.md.
 */
static const panel_btn_def_t PANEL_BUTTONS[] = {
    { .label = "BUTTON ONE", .type = PANEL_BTN_DIMMER, .instances = {0}, .instance_count = 1 },
    { .label = "BUTTON TWO", .type = PANEL_BTN_DIMMER, .instances = {0}, .instance_count = 1 },
    { .label = "BUTTON THREE", .type = PANEL_BTN_DIMMER, .instances = {0, 0}, .instance_count = 2 },
    { .label = "BUTTON FOUR", .type = PANEL_BTN_DIMMER, .instances = {0}, .instance_count = 1 },
    { .label = "BUTTON FIVE", .type = PANEL_BTN_DIMMER, .instances = {0}, .instance_count = 1 },
    { .label = "BUTTON SIX", .type = PANEL_BTN_DIMMER, .instances = {0}, .instance_count = 1 },
    { .label = "BUTTON SEVEN", .type = PANEL_BTN_SWITCH, .instances = {0, 0}, .instance_count = 2 },
};

#define PANEL_BUTTON_COUNT (sizeof(PANEL_BUTTONS) / sizeof(PANEL_BUTTONS[0]))
