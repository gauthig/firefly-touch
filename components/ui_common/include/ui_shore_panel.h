/*
 * ui_shore_panel — Line 1 / Line 2 shore-power readout, laid out like the
 * Hughes Autoformers phone app: two columns, each stacking Volts, Amps,
 * Freq and Watts, adapted to the panel's dark theme and portrait screen.
 *
 * Fed by telemetry broadcast from the basement BLE proxy (which holds the
 * BLE link to the Power Watchdog), NOT by a local connection -- see
 * components/hughes_watchdog and the broadcast telemetry notes in CLAUDE.md.
 *
 * A 30 A pedestal reports only Line 1; the Line 2 column is hidden rather
 * than shown as zeroes, since "0 V on L2" and "there is no L2" are very
 * different things to a reader.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t line_count;    /* 1 = 30 A service, 2 = 50 A */
    uint8_t error_code;    /* 0 = OK; non-zero colours the readout red */
    float   frequency_hz;
    float   volts[2];      /* index 0 = L1 */
    float   amps[2];
    float   watts[2];
} ui_shore_reading_t;

lv_obj_t *ui_shore_panel_create(lv_obj_t *parent);

/*
 * Push a reading. `valid` false shows "--" everywhere -- used when the
 * proxy's broadcasts have gone stale, so a dead link never leaves a frozen
 * reading looking live. Caller must hold the LVGL lock.
 */
void ui_shore_panel_set(lv_obj_t *panel, const ui_shore_reading_t *r, bool valid);

#ifdef __cplusplus
}
#endif
