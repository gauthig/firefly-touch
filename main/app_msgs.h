/*
 * app_msgs — message types carried on the inter-task queues.
 *
 *   UI (core 1)  --dimmer_cmd_msg_t-->  TX queue  -->  twai_tx_task (core 0)
 *   twai_rx_task (core 0)  --dimmer_status_msg_t-->  state manager (core 0)
 *   state manager  --ui_on_status() under LVGL lock-->  widgets (core 1)
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "rvc_protocol.h"

/* One command to one instance (multi-instance buttons enqueue one per). */
typedef struct {
    uint8_t          instance;
    rvc_dimmer_cmd_t command;
    uint8_t          level;      /* 0..200, or RVC_FIELD_NA */
    uint8_t          duration;   /* seconds, or RVC_FIELD_NA */
} dimmer_cmd_msg_t;

/* Decoded DC_DIMMER_STATUS_3, RX task -> state manager. */
typedef struct {
    uint8_t instance;
    uint8_t level;               /* operating level 0..200 */
    bool    on;
} dimmer_status_msg_t;

/* Decoded TANK_STATUS (SeeLevel II 709-RVC), RX task -> tank state manager.
 * Separate namespace from dimmer instances above -- never conflate the two. */
typedef struct {
    uint8_t instance;
    uint8_t percent;              /* 0..100, meaningful only if valid */
    bool    valid;                /* false if the sender reports "not available" */
} tank_status_msg_t;
