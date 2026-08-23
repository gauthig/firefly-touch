/*
 * jbd_bms_client — Bluedroid GATT-client central role for up to 3 fixed
 * Xiaoxiang/JBD BMS peripherals (v1: fixed MAC addresses from Kconfig, no
 * scanning/pairing UI, no mesh -- same v1 scoping as components/espnow_link's
 * single fixed ESP-NOW peer).
 *
 * Each configured slot runs its own connect/discover/subscribe state
 * machine and, once subscribed, periodically writes a basic-info request
 * and parses the notify response (jbd_bms_protocol.h) into a small state
 * table. A slot left at the Kconfig placeholder MAC
 * ("00:00:00:00:00:00") is skipped entirely.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "jbd_bms_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Brings up Bluedroid and starts connecting to each configured battery MAC.
 * Must be called after nvs_flash_init(); safe to call once per boot.
 */
esp_err_t jbd_bms_client_start(void);

/*
 * Last known status for battery `index` (0..2). Returns false if that
 * slot is unconfigured (placeholder MAC) or no valid reading has been
 * received yet -- mirrors state_manager_get_tank()'s valid/invalid
 * contract already used elsewhere in the UI.
 */
bool jbd_bms_get_status(uint8_t index, jbd_bms_status_t *out);

/*
 * True if slot `index` holds a real MAC (i.e. it is one this node is meant
 * to be talking to at all), regardless of whether it is currently
 * answering. Lets a caller tell "configured but offline" -- worth reporting
 * -- apart from "never configured", which should stay silent.
 */
bool jbd_bms_slot_configured(uint8_t index);

/* True if a valid notify was parsed for that slot recently -- the window is
 * derived from the poll interval (3x), not a fixed number of seconds. */
bool jbd_bms_healthy(uint8_t index);

#ifdef __cplusplus
}
#endif
