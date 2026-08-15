/*
 * panel_def — types used by the per-panel configuration headers in panels/.
 *
 * A panel header (selected at build time via idf.py -DPANEL=<name>) defines
 * PANEL_NAME, PANEL_INDEX and a PANEL_BUTTONS[] array of these structs laid
 * out in reading order for a 2-column x 4-row grid.
 *
 * A panel that sets PANEL_HAS_SCREEN_2 1 (main/panel_config.h) additionally
 * defines PANEL_BUTTONS_2[]/PANEL_BUTTON_COUNT_2, a second grid in the same
 * layout. One PANEL_BTN_SCREEN_SWITCH button per grid (conventionally the
 * same slot on both) flips between them — see main/ui/ui.c.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PANEL_BTN_MAX_INSTANCES 4

typedef enum {
    PANEL_BTN_DIMMER,         /* tap = toggle, hold = ramp, shows brightness bar */
    PANEL_BTN_SWITCH,         /* tap = on/off only */
    PANEL_BTN_SCREEN_SWITCH,  /* local UI nav only -- flips to the panel's screen 2 */
    PANEL_BTN_TANK_LEVEL,     /* read-only tank %, fed by TANK_STATUS, no tap action */
    PANEL_BTN_SPACER,         /* empty grid cell -- no widget, just holds the layout */
} panel_btn_type_t;

typedef struct {
    const char      *label;                              /* button caption */
    panel_btn_type_t type;
    uint8_t          instances[PANEL_BTN_MAX_INSTANCES]; /* RV-C instance(s) --
                                                              dimmer or tank
                                                              namespace, per type */
    uint8_t          instance_count;                     /* 0 for a non-RVC button */
} panel_btn_def_t;

#ifdef __cplusplus
}
#endif
