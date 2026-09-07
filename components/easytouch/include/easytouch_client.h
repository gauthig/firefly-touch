/*
 * easytouch_client — Bluedroid GATTC central for the Micro-Air EasyTouch RV
 * thermostat. Holds no standing connection: it connects, authenticates,
 * sends any queued Change, reads status, and DISCONNECTS every poll, so the
 * phone app stays usable between polls (docs/EASYTOUCH-THERMOSTAT.md §6).
 *
 * Runs on the hvac_panel node. Wire framing is components/easytouch's pure-C
 * codec (easytouch_protocol.h); this file is only the BLE transport, modelled
 * on jbd_bms_client.c / renogy_solar_client.c and the hvac_capture bench tool
 * (since removed -- issue #83).
 *
 * There is no command acknowledgement — confirmation of a Change is the next
 * status read, exactly like RV-C DC_DIMMER_STATUS_3. The panel's
 * status-driven-UI invariant applies unchanged: widgets reflect
 * easytouch_client_get_status(), never the local tap.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#include "easytouch_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Brings up ble_host, registers the GATTC app (BLE_HOST_APP_ID_EASYTOUCH),
 * and starts the poll task. Needs CONFIG_FIREFLY_EASYTOUCH_PASSWORD set (in
 * the per-build-dir sdkconfig, never sdkconfig.defaults); logs and idles if
 * it is empty.
 */
esp_err_t easytouch_client_start(void);

/*
 * Latest parsed status. Returns false (and leaves *out untouched) until the
 * first good status read. Mirrors jbd_bms_get_status()'s valid/invalid
 * contract.
 */
bool easytouch_client_get_status(easytouch_status_t *out);

/*
 * True while the last good status is fresher than 3x the poll interval —
 * same derived-window trick as jbd_bms_healthy(). The UI ages its readouts
 * to "--" when this goes false.
 */
bool easytouch_client_healthy(void);

/*
 * Queue a Change for the next poll connect. Coalescing is per zone: a second
 * submit for the same zone before the first is sent merges field-wise
 * (set_* flags OR together, values overwrite), so mashing +/- or cycling the
 * mode rapidly collapses to one write with the final values. Returns false
 * only if `change` is NULL or names a zone >= EASYTOUCH_MAX_ZONES.
 */
bool easytouch_client_submit_change(const easytouch_change_t *change);

#ifdef __cplusplus
}
#endif
