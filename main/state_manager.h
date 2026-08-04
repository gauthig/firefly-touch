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
