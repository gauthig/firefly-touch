/*
 * ui_thermostat — control surface for the Micro-Air EasyTouch RV thermostat
 * (hvac_panel only). One card per zone: inside temperature, selected mode
 * with a tap-to-cycle button, the mode's fan setting, and the mode's
 * setpoint with -/+.
 *
 * Fed by easytouch_status_t from the on-panel BLE client
 * (components/easytouch). Its displayed state moves ONLY on
 * ui_thermostat_set() -- a tap never flips the view optimistically; it
 * queues a Change and the next status read is what updates the card, exactly
 * like the dimmer buttons' status-driven-UI invariant.
 *
 * Commands leave through a single callback registered once with
 * ui_thermostat_set_cmd_cb() (there is only ever one thermostat widget). The
 * widget builds the easytouch_change_t; the callback owner decides how to
 * deliver it (the panel: easytouch_client_submit_change(); the simulator: a
 * fake). Per-zone off is mode = EASYTOUCH_MODE_OFF -- the widget never emits
 * a `power` key, which is system-wide.
 */
#pragma once

#include "lvgl.h"

#include "easytouch_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Zones rendered (this coach's 355 reports 3: Front / Mid Coach / Rear). */
#define UI_THERMOSTAT_ZONES 3

typedef void (*ui_thermostat_cmd_cb_t)(const easytouch_change_t *change);

/* Register the command sink. Shared by every ui_thermostat widget (there is
 * one). Call before creating the widget or any time after; NULL disables
 * command delivery (taps then do nothing). */
void ui_thermostat_set_cmd_cb(ui_thermostat_cmd_cb_t cb);

lv_obj_t *ui_thermostat_create(lv_obj_t *parent);

/* On-screen zone labels, in zone-index order. Any may be NULL to keep the
 * default ("ZONE 0" ...). Caller must hold the LVGL lock. */
void ui_thermostat_set_zone_names(lv_obj_t *w, const char *const names[UI_THERMOSTAT_ZONES]);

/*
 * Push a fresh reading. `valid` false (client stale / never seen) shows "--"
 * everywhere and disables the controls; `st` may be NULL then. Caller must
 * hold the LVGL lock.
 */
void ui_thermostat_set(lv_obj_t *w, const easytouch_status_t *st, bool valid);

#ifdef __cplusplus
}
#endif
