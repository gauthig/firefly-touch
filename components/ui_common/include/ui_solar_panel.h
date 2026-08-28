/*
 * ui_solar_panel — read-only Renogy MPPT solar readout.
 *
 * Fed by telemetry broadcast from the basement BLE proxy (which holds the
 * BLE link to the controller's BT-2 module), NOT by a local connection —
 * see components/renogy_solar and the broadcast telemetry notes in
 * CLAUDE.md. Same arrangement as ui_shore_panel.
 *
 * Layout: a header naming the section and the controller's charging state,
 * over a 3x2 grid of caption/value/unit tiles —
 *
 *     SOLAR                                    boost
 *     +-----------+-----------+-----------+
 *     |   Watts   | PV Volts  |  PV Amps  |
 *     +-----------+-----------+-----------+
 *     | Batt Volts| Ctrl Temp | Batt Temp |
 *     +-----------+-----------+-----------+
 *
 * A 3x2 grid rather than one row of six, because the widget has to look
 * right in two very different holes: a full landscape section on the 7"
 * main_cabinet, and a short strip under the battery bank on a 480px-wide
 * portrait panel. Six across would be unreadably narrow on the latter.
 *
 * ⚠️ Deliberately does NOT show state of charge, even though the controller
 * reports its own estimate. On the portrait panels this widget sits directly
 * beneath the battery bank readout, whose SOC comes from the BMS packs
 * themselves — two SOC numbers side by side, computed by different devices
 * from different data, would disagree sooner or later and there would be no
 * way for a reader to know which to believe. The BMS is the better source,
 * so it is the only one shown.
 *
 * TEMPERATURES ARRIVE ALREADY IN °F. The proxy converts once at the source
 * (see espnow_solar_msg_t), so nothing on the panel side needs to know the
 * controller reports Celsius.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t  charge_state;       /* renogy_charge_state_t: 2 = mppt, 5 = float */
    float    battery_volts;
    float    pv_volts;
    float    pv_amps;
    float    pv_watts;
    bool     temp_valid;
    float    controller_temp_f;
    float    battery_temp_f;
} ui_solar_reading_t;

lv_obj_t *ui_solar_panel_create(lv_obj_t *parent);

/*
 * Push a reading. `valid` false shows "--" everywhere — used when the
 * proxy's broadcasts have gone stale or it flagged the controller offline,
 * so a dead link never leaves a frozen reading looking live. Caller must
 * hold the LVGL lock.
 */
void ui_solar_panel_set(lv_obj_t *panel, const ui_solar_reading_t *r, bool valid);

#ifdef __cplusplus
}
#endif
