/*
 * hughes_wd_client — Bluedroid GATT-client for a Hughes Power Watchdog
 * Gen 1 (Bluetooth-only, EPO) surge protector.
 *
 * Differs from components/jbd_bms in three ways worth knowing:
 *
 *  1. It SCANS by advertised name instead of connecting to a fixed MAC.
 *     The batteries' MACs were known up front; the Watchdog's was not, and
 *     scanning also survives a unit swap. See CONFIG_FIREFLY_WD_NAME_MATCH.
 *     The scan itself belongs to ble_host, not to this client: there is one
 *     GAP scan per node and the solar client needs discovery too, so this
 *     client only declares WHAT it is looking for and is handed an address.
 *  2. It never polls. The device streams notifications ~1/second on its own
 *     once subscribed; there is no request frame and no way to slow it down.
 *  3. It is RECEIVE-ONLY. Gen 1 has no working command path -- see the
 *     NOTE ON CONTROL in hughes_wd_protocol.h.
 *
 * A 50 A unit sends a separate packet per line, so readings are kept in
 * per-line slots and jbd-style valid/stale contracts apply per line.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "hughes_wd_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Lines are addressed 1..2 to match the protocol's own numbering. */
#define HUGHES_WD_MAX_LINES 2u

/*
 * Brings up the BLE controller + Bluedroid, registers the GATTC app, and
 * starts scanning for a matching Watchdog. Safe to call once per boot,
 * after nvs_flash_init(). Returns as soon as the scan is under way --
 * connection happens asynchronously.
 */
esp_err_t hughes_wd_client_start(void);

/*
 * Latest reading for `line` (1 or 2). Returns false if that line has never
 * reported or its data is stale -- same valid/invalid contract as
 * jbd_bms_get_status()/state_manager_get_tank().
 */
bool hughes_wd_get_reading(uint8_t line, hughes_wd_reading_t *out);

/* True while connected AND at least one line reported recently. */
bool hughes_wd_healthy(void);

/*
 * How many lines this unit actually reports: 2 for a 50 A pedestal, 1 for a
 * 30 A one, 0 if nothing has been received yet. Derived from observed
 * traffic rather than configuration, since only the device knows.
 */
uint8_t hughes_wd_line_count(void);

/* Discovered peer address as a printable string ("--" until connected).
 * Logged at connect time so the MAC can be pinned later if wanted. */
const char *hughes_wd_peer_str(void);

#ifdef __cplusplus
}
#endif
