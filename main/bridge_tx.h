#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "rvc_protocol.h"

/*
 * Single injection point ui.c's panel_send_cb() uses to send a dimmer
 * command "out toward the bus". The concrete backend is selected at build
 * time by PANEL_HAS_CAN (panel_config.h): real CAN via
 * twai_enqueue_dimmer_cmd() on a CAN-connected panel, or an ESP-NOW frame
 * to the bridge panel on a remote (PANEL_HAS_CAN=0) panel. Same
 * non-blocking, drop-with-a-warning-if-full contract as
 * twai_enqueue_dimmer_cmd() either way — safe to call from the LVGL task.
 */
bool bridge_enqueue_dimmer_cmd(uint8_t instance, rvc_dimmer_cmd_t cmd,
                               uint8_t level, uint8_t duration);

/*
 * Group-addressed variant: instance 0xFF ("every instance in this group")
 * plus the group bitmask, which is how the coach's LIGHT MASTER rocker
 * drives all the lights at once. See docs/instance_map.yaml -> light_master.
 *
 * CAN panels only — it returns false on a remote panel, because group
 * addressing has no room in the ESP-NOW command frame. That is not a
 * limitation in practice: PANEL_BTN_LIGHT_MASTER is #error-guarded against
 * !PANEL_HAS_CAN already.
 */
bool bridge_enqueue_dimmer_group_cmd(uint8_t group, rvc_dimmer_cmd_t cmd,
                                     uint8_t level);

/*
 * DrainMaster valve command (PANEL_HAS_VALVE_CONTROL panels only). Unlike
 * the two functions above, this is UNCONDITIONAL -- not split by
 * PANEL_HAS_CAN -- because valve traffic never touches CAN at all in
 * either build: it always goes out over ESP-NOW to the valve node
 * (valves/), regardless of whether this panel also happens to have a CAN
 * bus. `action` is an espnow_valve_action_t value; kept as a plain uint8_t
 * here so this header stays free of espnow_link.h, same reason
 * bridge_enqueue_dimmer_cmd() takes rvc_dimmer_cmd_t rather than the wire
 * struct.
 */
bool bridge_enqueue_valve_cmd(uint8_t valve, uint8_t action);

/* Mirror espnow_valve_action_t's wire values, so a caller (ui.c) doesn't
 * need to include espnow_link.h just to name an action. Must stay in sync
 * with that enum -- both live in this one feature, not spread across
 * unrelated files, so there's one place to remember. */
#define BRIDGE_VALVE_ACTION_CLOSE 0u
#define BRIDGE_VALVE_ACTION_OPEN  1u
