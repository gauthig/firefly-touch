/* Simulator stub for jbd_bms_client.h -- no real Bluedroid BLE stack in the
 * sim, so this shadows the real header (which pulls in the ESP "bt" component)
 * with just the declarations ui.c needs. Backed by sim_stubs.c's fake
 * battery table, fed by its sweep timer -- same role as
 * state_manager.h's state_manager_get_tank() stub above. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "jbd_bms_protocol.h"

#define JBD_BMS_MAX_BATTERIES 3

bool jbd_bms_get_status(uint8_t index, jbd_bms_status_t *out);
bool jbd_bms_healthy(uint8_t index);
