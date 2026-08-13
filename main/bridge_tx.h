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
