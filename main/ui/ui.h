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
