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
 * This component owns (1) and (2): it brings the stack up exactly once and
 * registers the one real callback of each kind, then fans events out to
 * every registered observer. (3) stays the callers' problem — see
 * BLE_HOST_APP_ID_* below, which is where the allocation is recorded.
 *
 * Observers must tolerate events that are not theirs; the fan-out is
 * unconditional. Both clients here already filter (GATTC by gattc_if after
 * matching their own app_id at registration, and by remote address), which
 * is what makes plain fan-out safe.
 */
#pragma once

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
 *
 * Leave a gap after the batteries: their IDs are slot indices, so a fourth
 * pack would want 3.
 */
#define BLE_HOST_APP_ID_BATTERY_BASE 0
#define BLE_HOST_APP_ID_WATCHDOG     10

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

#ifdef __cplusplus
}
#endif
