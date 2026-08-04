#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "app_msgs.h"

/*
 * Creates the TX/status queues and starts twai_rx_task + twai_tx_task on
 * core 0, plus the state manager task. board_twai_init() must have been
 * called first.
 */
esp_err_t twai_tasks_start(void);

/*
 * Enqueue one dimmer command for transmit. Safe from any task including the
 * LVGL task (non-blocking; drops with a warning if the TX queue is full so
 * the UI can never stall on the bus).
 */
bool twai_enqueue_dimmer_cmd(uint8_t instance, rvc_dimmer_cmd_t cmd,
                             uint8_t level, uint8_t duration);
