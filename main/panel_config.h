/*
 * panel_config — indirection for the build-time panel selection.
 *
 * The root CMakeLists.txt turns `idf.py -DPANEL=<name>` into
 * PANEL_CONFIG_HEADER="<name>.h", resolved against the panels/ include dir.
 */
#pragma once

#ifndef PANEL_CONFIG_HEADER
#error "PANEL_CONFIG_HEADER is not defined — build with idf.py -DPANEL=<name>"
#endif

#include PANEL_CONFIG_HEADER

/*
 * RV-C source address for this panel, in the user/dynamic range.
 * TODO(bench): before deploying, sniff the bus and confirm 0x80+index does
 * not collide with any existing node (factory switches, Firefly gateway,
 * etc.). If it does, change the base here.
 */
#ifndef PANEL_SOURCE_ADDR
#define PANEL_SOURCE_ADDR ((uint8_t)(0x80 + PANEL_INDEX))
#endif

/*
 * PANEL_HAS_CAN 0 = this panel has no CAN wiring at all; it relays
 * commands/status to/from a PANEL_IS_BRIDGE panel over ESP-NOW instead
 * (components/espnow_link). Real (CAN-connected) panels default to 1.
 * PANEL_INDEX is still allocated from panels/REGISTRY.md even for a
 * PANEL_HAS_CAN=0 panel — it doubles as the ESP-NOW peer identity, keeping
 * one allocation table for every panel regardless of role.
 */
#ifndef PANEL_HAS_CAN
#define PANEL_HAS_CAN 1
#endif

/*
 * PANEL_IS_BRIDGE 1 = this panel also runs the ESP-NOW side, forwarding
 * remote-panel commands onto its own CAN bus and mirroring real status
 * back out. Only meaningful (and only ever needed) on a PANEL_HAS_CAN=1
 * panel; v1 supports exactly one bridge per remote panel.
 */
#ifndef PANEL_IS_BRIDGE
#define PANEL_IS_BRIDGE 0
#endif

#if PANEL_IS_BRIDGE && !PANEL_HAS_CAN
#error "PANEL_IS_BRIDGE requires PANEL_HAS_CAN (a bridge relays to a real CAN bus)"
#endif

/*
 * PANEL_HAS_SCREEN_2 1 = this panel defines a second button grid
 * (PANEL_BUTTONS_2[] / PANEL_BUTTON_COUNT_2) alongside its normal
 * PANEL_BUTTONS[], and should have exactly one PANEL_BTN_SCREEN_SWITCH
 * button somewhere in each grid to flip between them. See main/ui/ui.c.
 */
#ifndef PANEL_HAS_SCREEN_2
#define PANEL_HAS_SCREEN_2 0
#endif

/*
 * PANEL_HAS_SCREEN_3 1 = a third screen (PANEL_BUTTONS_3[] /
 * PANEL_BUTTON_COUNT_3). Requires PANEL_HAS_SCREEN_2 — screens are numbered
 * consecutively, and ui.c walks them as a list.
 *
 * With more than two screens, a PANEL_BTN_SCREEN_SWITCH button must say
 * WHICH screen it targets, via instances[0] (0 = the main button grid). A
 * button with instance_count == 0 keeps the original toggle-between-0-and-1
 * behaviour, so a two-screen panel needs no changes.
 */
#ifndef PANEL_HAS_SCREEN_3
#define PANEL_HAS_SCREEN_3 0
#endif

#if PANEL_HAS_SCREEN_3 && !PANEL_HAS_SCREEN_2
#error "PANEL_HAS_SCREEN_3 requires PANEL_HAS_SCREEN_2 (screens are consecutive)"
#endif

/*
 * PANEL_WANTS_TELEMETRY 1 = this CAN-connected panel also listens to the
 * ESP-NOW broadcast channel, in the receive-only TELEMETRY role (no unicast
 * peer, no keys, nothing actuable).
 *
 * Needed because some readings exist ONLY as broadcasts: the battery packs
 * and the Hughes Power Watchdog are on BLE links held by the basement
 * proxy, and no amount of CAN wiring reaches them. A remote panel gets this
 * for free as part of PANEL_HAS_CAN=0; a CAN panel that wants a battery or
 * shore-power section has to ask.
 */
#ifndef PANEL_WANTS_TELEMETRY
#define PANEL_WANTS_TELEMETRY 0
#endif

#if PANEL_WANTS_TELEMETRY && !PANEL_HAS_CAN
#error "PANEL_WANTS_TELEMETRY is for CAN panels; a remote panel already receives telemetry"
#endif

/*
 * PANEL_HAS_NAV_RAIL 1 = this panel shows a PERSISTENT side rail listing its
 * sections, with the selected one filling the rest of the screen, instead of
 * the whole-screen swap the other panels use. The panel header then defines
 * PANEL_NAV_RAIL[]/PANEL_NAV_RAIL_COUNT — plain panel_btn_def_t entries of
 * type PANEL_BTN_SCREEN_SWITCH, reusing the existing convention that
 * instances[0] names the target screen.
 *
 * Rail order is independent of screen index, so a panel can list its
 * sections in whatever order reads best while keeping its button grid on
 * screen 0 (where build_button_grid() handles it).
 *
 * This wants a landscape display; a portrait panel has no width to spare.
 */
#ifndef PANEL_HAS_NAV_RAIL
#define PANEL_HAS_NAV_RAIL 0
#endif

#if PANEL_HAS_NAV_RAIL && !PANEL_HAS_SCREEN_2
#error "PANEL_HAS_NAV_RAIL needs at least two screens to navigate between"
#endif

/*
 * Screen shown at boot, and returned to when the backlight idles off. 0 is
 * the main button grid. A rail panel typically wants a different one — the
 * grid is just one section among several, not the home screen.
 */
#ifndef PANEL_DEFAULT_SCREEN
#define PANEL_DEFAULT_SCREEN 0
#endif

/*
 * Columns in the main button grid. Two suits a portrait 4.3" panel; a wide
 * landscape panel fits more across and would otherwise stretch each button
 * into a letterbox.
 */
#ifndef PANEL_GRID_COLS
#define PANEL_GRID_COLS 2
#endif

/*
 * No panel runs a BLE client any more. The three JBD/Xiaoxiang battery
 * packs sit in the basement bay next to the headless proxy (proxy/), which
 * holds their BLE links and broadcasts the readings over ESP-NOW; a panel
 * just displays what arrives. The old PANEL_HAS_BLE_BATTERY switch is gone
 * rather than left as an unused option, since nothing selects it and the
 * radio-planning tradeoff behind the move is not one to re-litigate per
 * panel.
 */
