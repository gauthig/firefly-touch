/*
 * ui_battery_gauge — plain-box battery status readout (SOC%, charge/
 * discharge rate + word, ETA), styled like the other button cards (no
 * animation, no battery-silhouette graphic -- just background + text).
 * Deliberately takes plain numbers rather than a jbd_bms_status_t so
 * ui_common has no dependency on the jbd_bms component -- the caller
 * (main/ui/ui.c) does that conversion.
 *
 * Tapping the box (routed from ui_dimmer_button.c) toggles a small popup
 * showing the battery's configured BLE MAC address, for telling which
 * physical battery a slot corresponds to; tapping the popup dismisses it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *ui_battery_gauge_create(lv_obj_t *parent);

/*
 * Update the displayed reading. valid=false shows "--" everywhere instead
 * of a stale or fabricated value. rate_amps: positive = charging, negative
 * = discharging (same convention as jbd_bms_status_t.current_a) -- shown
 * with a "Charging"/"Discharging"/"Idle" word underneath. hours: pass a
 * non-finite value (e.g. INFINITY) to show "--" for the ETA. Caller must
 * hold the LVGL lock.
 */
void ui_battery_gauge_set_status(lv_obj_t *gauge, uint8_t percent, float rate_amps,
                                 float hours, bool valid);

/* BLE MAC string shown in the troubleshooting popup below. Safe to call
 * any time after create; NULL/empty shows "--". */
void ui_battery_gauge_set_mac(lv_obj_t *gauge, const char *mac);

/* Toggles the MAC popup open/closed. Tapping the popup itself also closes
 * it. Caller must hold the LVGL lock. */
void ui_battery_gauge_toggle_mac_popup(lv_obj_t *gauge);

#ifdef __cplusplus
}
#endif
