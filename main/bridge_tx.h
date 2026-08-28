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
