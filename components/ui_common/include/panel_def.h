/*
 * panel_def — types used by the per-panel configuration headers in panels/.
 *
 * A panel header (selected at build time via idf.py -DPANEL=<name>) defines
 * PANEL_NAME, PANEL_INDEX and a PANEL_BUTTONS[] array of these structs laid
 * out in reading order for a PANEL_GRID_COLS-wide grid (2 by default).
 *
 * A panel that sets PANEL_HAS_SCREEN_2 1 (main/panel_config.h) additionally
 * defines PANEL_BUTTONS_2[]/PANEL_BUTTON_COUNT_2, and optionally a third
 * screen. One PANEL_BTN_SCREEN_SWITCH button per grid flips between them —
 * or, on a panel with PANEL_HAS_NAV_RAIL, a persistent side rail does the
 * navigating instead. See main/ui/ui.c.
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
    /*
     * Local UI nav only, never sent to the bus. instances[0] is reused as
     * the TARGET SCREEN INDEX (0 = the main button grid), the same way
     * PANEL_BTN_BATTERY_SUMMARY-era buttons reused it as a non-RV-C index.
     * instance_count == 0 keeps the original two-screen behaviour: toggle
     * between screen 0 and screen 1. That is what lets a panel with only
     * one secondary screen stay unchanged. Also the entry type used by
     * PANEL_NAV_RAIL[] on a side-nav panel.
     */
    PANEL_BTN_SCREEN_SWITCH,
    PANEL_BTN_TANK_LEVEL,     /* read-only tank %, fed by TANK_STATUS, no tap action */
    PANEL_BTN_BATTERY_SUMMARY,/* read-only combined readout for the whole battery
                                  bank, fed by jbd_bms_combine() over every
                                  configured pack. Takes no instances -- the packs
                                  are BLE slots, not RV-C instances. Tap toggles a
                                  per-pack detail popup. */
    PANEL_BTN_SHORE_POWER,    /* read-only shore-power readout (Hughes Power Watchdog,
                                  relayed from the basement proxy): Line 1 / Line 2
                                  volts, amps, frequency and watts. Takes no instances */
    /*
     * Purely local two-state button: shows `label` when off and `label_alt`
     * when on, and sends NOTHING to the bus. Used for controls whose real
     * actuation isn't built yet (the grey/black dump valves and the
     * gravity/macerator selector on main_cabinet), so the panel can carry
     * the control surface before the plumbing exists. State is in memory
     * only and resets on reboot -- correct for a button that drives nothing.
     */
    PANEL_BTN_LOCAL_TOGGLE,
    /*
     * All-lights master. Takes no instances: it reads "is any light on?"
     * from the state manager and sweeps every instance currently reporting
     * on when switched off. Turning ON uses the panel header's
     * PANEL_MASTER_ON[] list, because RV-C has no all-on command and the
     * G6's own LIGHT MASTER DGN has never been captured (see
     * docs/instance_map.yaml). No dimming or ramp -- tap only.
     */
    PANEL_BTN_LIGHT_MASTER,
    PANEL_BTN_SPACER,         /* empty grid cell -- no widget, just holds the layout */
} panel_btn_type_t;

typedef struct {
    const char      *label;                              /* button caption */
    panel_btn_type_t type;
    uint8_t          instances[PANEL_BTN_MAX_INSTANCES]; /* RV-C instance(s) --
                                                              dimmer or tank
                                                              namespace, per type */
    uint8_t          instance_count;                     /* 0 for a non-RVC button */
    /*
     * PANEL_BTN_LOCAL_TOGGLE only: the caption shown in the on state.
     * Deliberately the LAST field so every existing positional initializer
     * in every panels/ header keeps compiling and zero-fills it to NULL.
     */
    const char      *label_alt;
} panel_btn_def_t;

#ifdef __cplusplus
}
#endif
