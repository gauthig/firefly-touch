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
