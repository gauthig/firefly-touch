/*
 * panel_def — types used by the per-panel configuration headers in panels/.
 *
 * A panel header (selected at build time via idf.py -DPANEL=<name>) defines
 * PANEL_NAME, PANEL_INDEX and a PANEL_BUTTONS[] array of these structs laid
 * out in reading order for a 2-column x 4-row grid.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PANEL_BTN_MAX_INSTANCES 4

typedef enum {
    PANEL_BTN_DIMMER,        /* tap = toggle, hold = ramp, shows brightness bar */
    PANEL_BTN_SWITCH,        /* tap = on/off only */
    PANEL_BTN_PANEL_LIGHTS,  /* local placeholder — see Note B in CLAUDE.md */
} panel_btn_type_t;

typedef struct {
    const char      *label;                              /* button caption */
    const char      *symbol;                             /* LVGL symbol string (LV_SYMBOL_*) */
    panel_btn_type_t type;
    uint8_t          instances[PANEL_BTN_MAX_INSTANCES]; /* RV-C dimmer instances */
    uint8_t          instance_count;                     /* 0 for PANEL_LIGHTS */
} panel_btn_def_t;

#ifdef __cplusplus
}
#endif
