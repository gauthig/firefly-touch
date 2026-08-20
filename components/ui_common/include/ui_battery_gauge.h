/*
 * ui_battery_gauge — animated battery-shaped SOC gauge with rate/ETA text.
 *
 * A rounded battery-silhouette "glass" (body + top nub) filled to the
 * current SOC percent with the same slowly scrolling sine-wave surface as
 * ui_tank_wave, colored by SOC band (green/warn/err), plus a percent label
 * on the body and two text labels below it for charge/discharge rate and
 * estimated remaining hours. Deliberately takes plain numbers rather than a
 * jbd_bms_status_t so ui_common has no dependency on the jbd_bms component
 * -- the caller (main/ui/ui.c) does that conversion.
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
 * = discharging (same convention as jbd_bms_status_t.current_a). hours:
 * pass a non-finite value (e.g. INFINITY) to show "--" for the ETA.
 * Caller must hold the LVGL lock.
 */
void ui_battery_gauge_set_status(lv_obj_t *gauge, uint8_t percent, float rate_amps,
                                 float hours, bool valid);

#ifdef __cplusplus
}
#endif
