#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "panel_config.h"
#include "rvc_protocol.h"

#if PANEL_HAS_THERMOSTAT
#include "easytouch_protocol.h"
#endif

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

#if PANEL_HAS_THERMOSTAT
/*
 * Single injection point ui_thermostat's controls use to apply a change.
 * Resolves at build time:
 *   - PANEL_HAS_EASYTOUCH  -> easytouch_client_submit_change() (local BLE)
 *   - PANEL_WANTS_HVAC_CONTROL -> espnow_link_send_hvac_cmd() to hvac_panel
 * Only the fields whose set_* flag is set are acted on; over ESP-NOW each
 * becomes its own tiny frame (mode / cool_sp / heat_sp). Safe from the LVGL
 * task. Returns false if there is no delivery path configured.
 */
bool bridge_enqueue_hvac_change(const easytouch_change_t *change);
#endif
