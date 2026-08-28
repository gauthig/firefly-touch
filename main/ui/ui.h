#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Builds the panel screen (status bar + button grid). Takes the LVGL lock
 * itself; call after board_display_init(). */
void ui_init(void);

/*
 * Push a status change into the UI. Called by the state manager (core 0);
 * takes the LVGL lock internally. Safe before ui_init (no-op).
 */
void ui_on_status(uint8_t instance, uint8_t level, bool on);

/*
 * Push a tank-level update into the UI. Called by the tank state manager
 * (core 0); takes the LVGL lock internally. Safe before ui_init (no-op).
 * Separate from ui_on_status() -- tank instances are a different namespace,
 * never routed through the dimmer update path.
 */
void ui_on_tank_status(uint8_t instance, uint8_t percent, bool valid);

/*
 * Shore-power readings relayed from the basement BLE proxy (a Hughes Power
 * Watchdog), one entry per line. Deliberately a plain struct of floats
 * rather than the espnow wire type, so ui.h stays free of espnow_link.h --
 * main.c converts. Index 0 = L1, 1 = L2; line_count is 1 on a 30 A service.
 */
typedef struct {
    uint8_t line_count;
    uint8_t error_code;    /* 0 = OK; see hughes_wd_error_str() */
    float   frequency_hz;
    float   volts[2];
    float   amps[2];
    float   watts[2];
} ui_shore_power_t;

/*
 * Push a shore-power update into the UI. Takes the LVGL lock internally.
 * The UI ages these out on its own if broadcasts stop, so there is no
 * "invalid" call -- silence is the invalid signal.
 */
void ui_on_shore_power(const ui_shore_power_t *sp);

/*
 * One battery pack's reading, relayed from the basement BLE proxy. Same
 * shape of contract as ui_shore_power_t: a plain struct rather than the
 * espnow wire type, so ui.h stays free of espnow_link.h and main.c does the
 * unscaling.
 *
 * The packs are wired in parallel and display as ONE bank, but they arrive
 * (and are stored) separately — the UI runs jbd_bms_combine() over whichever
 * ones are currently fresh, so a pack dropping off shrinks the bank instead
 * of dragging its averages toward zero, and the per-pack detail popup still
 * has real per-pack numbers to show.
 *
 * Temperatures are this pack's own min/max across its NTC probes, which is
 * all the popup and the bank's high/low strip ever display.
 */
typedef struct {
    uint8_t slot;              /* 0-based battery index */
    bool    online;            /* false = producer says this pack is not answering */
    uint8_t soc_percent;
    float   voltage_v;
    float   current_a;         /* signed: positive = charging */
    float   residual_ah;
    float   full_capacity_ah;
    bool    temp_valid;
    float   temp_min_c;
    float   temp_max_c;
} ui_battery_pack_t;

/*
 * Push one pack's reading into the UI. Takes the LVGL lock internally.
 * Like shore power, the UI ages these out on its own, so silence — not an
 * explicit invalid call — is what marks a pack offline.
 */
void ui_on_battery_status(const ui_battery_pack_t *pack);

/*
 * The Renogy MPPT solar controller's reading, relayed from the basement BLE
 * proxy. Same contract as ui_shore_power_t: a plain struct rather than the
 * espnow wire type, so ui.h stays free of espnow_link.h and main.c does the
 * unscaling.
 *
 * TEMPERATURES ARE ALREADY IN DEGREES F -- converted once on the proxy (see
 * espnow_solar_msg_t), so no consumer has to know the controller itself
 * reports Celsius.
 */
typedef struct {
    bool    online;            /* false = producer says the controller is not answering */
    uint8_t charge_state;      /* renogy_charge_state_t: 2 = mppt, 5 = float */
    uint8_t battery_soc;       /* the controller's own estimate; not displayed --
                                  see the note in ui_solar_panel.h */
    float   battery_volts;
    float   pv_volts;
    float   pv_amps;
    float   pv_watts;
    bool    temp_valid;
    float   controller_temp_f;
    float   battery_temp_f;
} ui_solar_status_t;

/*
 * Push a solar update into the UI. Takes the LVGL lock internally. Like
 * shore power, the UI ages these out on its own, so silence -- not an
 * explicit invalid call -- is what marks the controller gone.
 */
void ui_on_solar_status(const ui_solar_status_t *solar);
