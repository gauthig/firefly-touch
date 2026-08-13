/*
 * espnow_link — peer-to-peer ESP-NOW link between a CAN-connected "bridge"
 * panel and a remote panel with no CAN wiring at all.
 *
 * v1 is a single fixed peer (MAC + PMK/LMK from Kconfig, see
 * Kconfig.projbuild) on both ends — no runtime pairing, no mesh, no more
 * than one remote per bridge. A bridge panel calls espnow_link_init(true)
 * and uses espnow_link_send_status()/espnow_link_set_cmd_rx_cb(); a remote
 * panel calls espnow_link_init(false) and uses
 * espnow_link_send_cmd()/espnow_link_set_status_rx_cb().
 *
 * Mirrors the twai_tasks.c producer/consumer split: the ESP-NOW recv
 * callback (WiFi driver task context) only posts to a queue, never touches
 * application state directly — espnow_rx_task drains it and invokes the
 * registered callback.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "rvc_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Mirrors dimmer_cmd_msg_t (main/app_msgs.h) — kept separate so this
 * component has no dependency on main/. */
typedef struct {
    uint8_t          instance;
    rvc_dimmer_cmd_t command;
    uint8_t          level;      /* 0..200, or RVC_FIELD_NA */
    uint8_t          duration;   /* seconds, or RVC_FIELD_NA */
} espnow_cmd_msg_t;

/* Mirrors dimmer_status_msg_t. */
typedef struct {
    uint8_t instance;
    uint8_t level;
    bool    on;
} espnow_status_msg_t;

typedef void (*espnow_cmd_rx_cb_t)(const espnow_cmd_msg_t *msg, void *ctx);
typedef void (*espnow_status_rx_cb_t)(const espnow_status_msg_t *msg, void *ctx);

/*
 * Brings up WiFi in STA-no-connect mode + ESP-NOW, adds the single peer
 * from CONFIG_FIREFLY_ESPNOW_PEER_MAC, and starts espnow_rx_task. Must be
 * called after nvs/board init; safe to call once per boot.
 */
esp_err_t espnow_link_init(bool is_bridge);

/* Remote panel -> bridge: send a dimmer command to be relayed onto CAN. */
bool espnow_link_send_cmd(const espnow_cmd_msg_t *msg);

/* Bridge -> remote panel: relay a real DC_DIMMER_STATUS_3 change. */
bool espnow_link_send_status(const espnow_status_msg_t *msg);

/* True if any frame was received from the peer within the last 5 s. */
bool espnow_link_healthy(void);

/* Bridge role: called from espnow_rx_task when a command arrives. */
void espnow_link_set_cmd_rx_cb(espnow_cmd_rx_cb_t cb, void *ctx);

/* Remote role: called from espnow_rx_task when a status update arrives. */
void espnow_link_set_status_rx_cb(espnow_status_rx_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
