/*
 * ble_host — shared Bluedroid bring-up and callback fan-out, so more than
 * one BLE client can live on the same node.
 *
 * Bluedroid is stubbornly single-tenant in three places, and every one of
 * them is a silent failure rather than an error:
 *
 *   1. esp_bt_controller_init()/esp_bluedroid_init() are one-shot. A second
 *      client calling them gets ESP_ERR_INVALID_STATE back, which reads
 *      like a real failure and (under ESP_ERROR_CHECK) aborts the boot.
 *   2. esp_ble_gap_register_callback() and esp_ble_gattc_register_callback()
 *      each hold exactly ONE function pointer. Registering a second
 *      callback does not fail — it REPLACES the first, and the client that
 *      registered earlier simply stops receiving events forever.
 *   3. GATTC app IDs are a single flat namespace across the whole node, so
 *      two clients that both start counting from 0 collide.
 *
 *   4. The GAP SCAN is node-wide, not per-client.
 *      esp_ble_gap_set_scan_params()/start_scanning()/stop_scanning() are
 *      one global scan. Two clients that each scan for their own device
 *      stomp each other's parameters, and whichever finds its device first
 *      calls stop_scanning() and cancels the OTHER client's scan too — so
 *      the second device is simply never discovered, with nothing logged.
 *
 * This component owns (1), (2) and (4): it brings the stack up exactly
 * once, registers the one real callback of each kind and fans events out to
 * every registered observer, and runs the one scan on behalf of everybody
 * who needs discovery. (3) stays the callers' problem — see
 * BLE_HOST_APP_ID_* below, which is where the allocation is recorded.
 *
 * Observers must tolerate events that are not theirs; the fan-out is
 * unconditional. Every client here already filters (GATTC by gattc_if after
 * matching their own app_id at registration, and by remote address), which
 * is what makes plain fan-out safe.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * GATTC app-ID allocation for this node. One flat namespace shared by every
 * client, so it is recorded in one place rather than each client picking a
 * number and hoping.
 *
 *   0, 1, 2  jbd_bms_client — one app per battery slot, app_id == slot index
 *   10       hughes_wd_client — the Power Watchdog
 *   20       renogy_solar_client — the MPPT charge controller via its BT-2
 *
 * Leave a gap after the batteries: their IDs are slot indices, so a fourth
 * pack would want 3. The others are spaced by 10 for the same reason —
 * room to grow without renumbering anything already deployed.
 */
#define BLE_HOST_APP_ID_BATTERY_BASE 0
#define BLE_HOST_APP_ID_WATCHDOG     10
#define BLE_HOST_APP_ID_SOLAR        20

/*
 * Brings up the BT controller (BLE mode) and Bluedroid, and registers the
 * shared GAP/GATTC callbacks. Idempotent: safe for every client to call at
 * start-up without caring who got there first.
 */
esp_err_t ble_host_start(void);

/*
 * Add an observer. Both may be called before or after ble_host_start().
 * Returns ESP_ERR_NO_MEM if the observer table is full.
 */
esp_err_t ble_host_add_gap_observer(esp_gap_ble_cb_t cb);
esp_err_t ble_host_add_gattc_observer(esp_gattc_cb_t cb);

/* ------------------------------------------------- scan arbitration ----- *
 *
 * There is exactly ONE GAP scan per node (see (4) above), so clients do not
 * drive it themselves — they describe the device they are looking for and
 * ble_host runs the scan on everybody's behalf.
 *
 * ⚠️ A client must never call esp_ble_gap_set_scan_params(),
 * esp_ble_gap_start_scanning() or esp_ble_gap_stop_scanning() directly, for
 * the same reason it must never call esp_ble_gap_register_callback():
 * it works right up until a second client does it too, and then fails
 * silently.
 *
 * Lifecycle:
 *   - register a matcher once at start-up;
 *   - call ble_host_scan_want(h, true) whenever you need an address —
 *     at start-up, and again after a disconnect or a failed connect;
 *   - your `found` callback fires once the scan has actually STOPPED, so it
 *     is safe to open a connection from inside it;
 *   - `want` is cleared automatically when your matcher hits. Set it again
 *     yourself if the connection then falls over.
 *
 * ble_host scans in bounded windows and restarts them while anyone is still
 * waiting, so a device that is powered up later is still found.
 */

/* Called for each advertisement seen while a scan is running, with the
 * advertised name already resolved to a NUL-terminated string. Return true
 * if this is the device you are looking for. Must not block. */
typedef bool (*ble_host_scan_match_t)(const char *name, void *ctx);

/* Called once, after scanning has stopped, with the address your matcher
 * accepted. Opening a GATT connection from here is the intended use. */
typedef void (*ble_host_scan_found_t)(const esp_bd_addr_t bda,
                                      esp_ble_addr_type_t addr_type, void *ctx);

/* Registers a matcher. Returns a non-negative handle for
 * ble_host_scan_want(), or -1 if the matcher table is full. Registration
 * alone does not start a scan — ask for one with ble_host_scan_want(). */
int ble_host_scan_add_matcher(ble_host_scan_match_t match,
                              ble_host_scan_found_t found, void *ctx);

/* Declares whether `handle` still needs an address. Scanning runs whenever
 * at least one matcher wants one and stops when none do. Safe to call
 * before ble_host_start(); the scan begins when the stack is up. */
void ble_host_scan_want(int handle, bool want);

#ifdef __cplusplus
}
#endif
