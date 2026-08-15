#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/*
 * State manager: owns the instance -> {level, on} table. Consumes decoded
 * DC_DIMMER_STATUS_3 messages from the RX task and pushes changes to the UI
 * (under the LVGL lock). Runs on core 0.
 */
esp_err_t state_manager_start(QueueHandle_t status_queue);

/* Called by the RX task on every received frame (any DGN): bus liveness. */
void state_manager_note_rx(void);

/* True if any frame was seen on the bus within the last 5 seconds. */
bool state_manager_bus_healthy(void);

/* Last known state for an instance; returns false if never reported. */
bool state_manager_get(uint8_t instance, uint8_t *level, bool *on);

typedef void (*state_status_sink_t)(uint8_t instance, uint8_t level, bool on, void *ctx);

/*
 * Register an additional sink notified alongside ui_on_status() whenever an
 * instance's state changes (v1: at most one sink — the bridge panel's
 * ESP-NOW forwarder; registering again replaces the previous one).
 */
void state_manager_register_status_sink(state_status_sink_t cb, void *ctx);

/*
 * Invoke cb once for every instance with known state. Used to push a full
 * resync — e.g. to a newly (re)connected ESP-NOW remote panel that would
 * otherwise wait for the next incidental bus change to populate its UI.
 */
void state_manager_for_each_known(state_status_sink_t cb, void *ctx);

/*
 * Tank sensor state (Garnet SeeLevel II TANK_STATUS, DGN 0x1FFB7) — a
 * separate, smaller table from the dimmer instance table above. Tank
 * instance numbers (0=fresh, 1=black, 2=gray, ...) are a different
 * namespace under a different DGN and must never be conflated with dimmer
 * instance 0. Runs its own task, consuming its own queue.
 */
esp_err_t state_manager_start_tanks(QueueHandle_t tank_status_queue);

/* Last known percent for a tank instance; returns false if never reported
 * or the sender's own last report was "not available". */
bool state_manager_get_tank(uint8_t instance, uint8_t *percent);
