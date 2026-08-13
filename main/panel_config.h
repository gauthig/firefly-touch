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
