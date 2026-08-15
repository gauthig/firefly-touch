/*
 * ui_tank_wave — animated water-wave tank-level gauge.
 *
 * A rounded "glass" tank filled to the current percent, with a slowly
 * scrolling sine-wave surface (not a plain flat bar) and the percent number
 * overlaid. Read-only, no tap action -- driven entirely by
 * ui_tank_wave_set_percent().
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *ui_tank_wave_create(lv_obj_t *parent);

/*
 * Update the displayed level. valid=false shows "--" and an empty tank
 * instead of a stale or fabricated percent. Caller must hold the LVGL lock.
 */
void ui_tank_wave_set_percent(lv_obj_t *wave, uint8_t percent, bool valid);

#ifdef __cplusplus
}
#endif
