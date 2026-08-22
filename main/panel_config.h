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
 * PANEL_HAS_BLE_BATTERY 1 = this panel starts the jbd_bms BLE client
 * (components/jbd_bms) at boot and shows a battery-status screen fed by
 * it. The jbd_bms component is always compiled into main (see
 * main/CMakeLists.txt, same precedent as espnow_link being required
 * unconditionally even though only some panels use it) but the BLE/
 * Bluedroid BLE stack is only actually started when this is set, so a
 * CAN-only panel like mid_coach never pays the coexistence/runtime cost.
 */
#ifndef PANEL_HAS_BLE_BATTERY
#define PANEL_HAS_BLE_BATTERY 0
#endif
