/*
 * valve_drive — ESP-NOW-commanded OPEN/CLOSE state machine, on top of the
 * relay/interlock/watchdog primitives in components/valve_control.
 *
 * Producer/consumer split, same shape as twai_rx/tx and espnow_rx_task
 * elsewhere in this codebase: the ESP-NOW cmd_rx callback (called from
 * espnow_rx_task context) only enqueues a command and returns immediately;
 * a dedicated task drains the queue and actually drives, so a slow drive
 * never blocks the ESP-NOW receive path. The queue itself is what
 * serializes drives -- a command for the other valve just waits its turn
 * rather than needing a separate "busy" flag.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Registers the ESP-NOW valve-command callback, seeds each valve's reported
 * position from a fresh DI read (never assumed closed, per
 * docs/DRAINMASTER-VALVES.md #7 "Startup"), starts the drive task and the
 * periodic status resync timer.
 *
 * Call after BOTH valve_control_driver_init() (the relay/DI driver must
 * already be up) AND espnow_link_init(ESPNOW_ROLE_VALVE_NODE) (this
 * registers a callback on that link).
 */
esp_err_t valve_drive_init(void);

#ifdef __cplusplus
}
#endif
