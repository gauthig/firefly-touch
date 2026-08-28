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

/*
 * One command to one instance (multi-instance buttons enqueue one per), or —
 * when `group` is not RVC_FIELD_NA — one command to a whole GROUP, with
 * instance set to 0xFF meaning "all instances in this group". Group
 * addressing is what the coach's own LIGHT MASTER rocker uses; see
 * docs/instance_map.yaml -> light_master.
 *
 * ⚠️ This struct is main-local. components/espnow_link deliberately mirrors
 * it with its OWN struct rather than sharing this one, and that frame's size
 * is pinned by a _Static_assert — so adding a field here does not touch the
 * ESP-NOW wire format. Keep it that way.
 */
typedef struct {
    uint8_t          instance;
    rvc_dimmer_cmd_t command;
    uint8_t          level;      /* 0..200, RVC_LEVEL_RESTORE, or RVC_FIELD_NA */
    uint8_t          duration;   /* seconds, or RVC_FIELD_NA */
    uint8_t          group;      /* group bitmask, or RVC_FIELD_NA for none */
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
