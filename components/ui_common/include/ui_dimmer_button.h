/*
 * ui_dimmer_button — shared button widget for dimmer / switch / screen-nav /
 * tank-level loads.
 *
 * Interaction model:
 *   tap             -> toggle (single-instance dimmers send TOGGLE; multi-
 *                      instance buttons and switches send OFF-to-all if any
 *                      instance is on, else ON-to-all, so grouped loads
 *                      never desync)
 *   press-and-hold  -> ramp (dimmers only): RAMP_UP or RAMP_DOWN sent on
 *                      long-press and re-sent while held, STOP on release;
 *                      direction alternates hold-to-hold, up when off
 *
 * PANEL_BTN_SCREEN_SWITCH taps only flip screens locally (see main/ui/ui.c);
 * PANEL_BTN_TANK_LEVEL is entirely read-only, no tap action at all.
 *
 * Visual state for dimmer/switch buttons is driven ONLY through
 * ui_dimmer_button_update() — i.e. only by DC_DIMMER_STATUS_3 frames from
 * the bus. Tank-level buttons are driven ONLY through
 * ui_dimmer_button_update_tank() — i.e. only by TANK_STATUS frames, a
 * separate DGN/namespace that never crosses over with the dimmer one.
 * Sending a command never changes a widget locally, so this panel stays
 * consistent with factory switches and the Firefly app.
 */
#pragma once

#include "lvgl.h"
#include "panel_def.h"
#include "rvc_protocol.h"
#include "ui_battery_summary.h"
#include "ui_shore_panel.h"
#include "ui_solar_panel.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Called when the widget wants to emit a command. The receiver fans the
 * command out to def->instances. Runs in LVGL task context — must not
 * block; post to a queue.
 */
typedef void (*ui_dimmer_send_cb_t)(const panel_btn_def_t *def,
                                    rvc_dimmer_cmd_t cmd,
                                    void *user_ctx);

lv_obj_t *ui_dimmer_button_create(lv_obj_t *parent,
                                  const panel_btn_def_t *def,
                                  ui_dimmer_send_cb_t send_cb,
                                  void *user_ctx);

/*
 * Feed a status update for one RV-C instance. If this widget doesn't watch
 * that instance, or isn't a PANEL_BTN_DIMMER/PANEL_BTN_SWITCH, the call is a
 * no-op (tank-level widgets use ui_dimmer_button_update_tank() instead —
 * a different DGN/namespace, deliberately never routed through this one).
 * Shows ON if any watched instance is on; brightness bar shows the highest
 * reported level. Caller must hold the LVGL lock.
 */
void ui_dimmer_button_update(lv_obj_t *btn, uint8_t instance,
                             uint8_t level, bool on);

/*
 * Feed a tank-level update for one TANK_STATUS instance. No-op unless this
 * widget is a PANEL_BTN_TANK_LEVEL watching that instance. valid=false shows
 * "--" instead of a stale or fabricated 0%. Caller must hold the LVGL lock.
 */
void ui_dimmer_button_update_tank(lv_obj_t *btn, uint8_t instance,
                                  uint8_t percent, bool valid);

/*
 * Feed a combined bank reading to a PANEL_BTN_BATTERY_SUMMARY widget. No-op
 * for any other button type. Caller must hold the LVGL lock.
 *
 * `bank` may be NULL (or carry pack_count == 0) to show "--" rather than a
 * stale reading. `packs`/`packs_len` back the tap-to-reveal per-pack detail
 * popup and are indexed by BLE SLOT, including unconfigured ones.
 * `configured_packs` is the tally of slots holding a real MAC, used for the
 * "N of M" indicator -- deliberately separate from packs_len, since slot 2
 * can be configured while slot 1 is not.
 */
void ui_dimmer_button_update_bank(lv_obj_t *btn, const jbd_bms_bank_t *bank,
                                  const ui_battery_pack_info_t *packs,
                                  uint8_t packs_len, uint8_t configured_packs);

/*
 * Feed a shore-power reading to a PANEL_BTN_SHORE_POWER widget. No-op for
 * any other button type. `valid` false shows "--" everywhere. Caller must
 * hold the LVGL lock.
 */
/*
 * Feed a solar reading to a PANEL_BTN_SOLAR widget. No-op for any other
 * button type, so callers can sweep every button without checking.
 */
void ui_dimmer_button_update_solar(lv_obj_t *btn, const ui_solar_reading_t *r,
                                   bool valid);

void ui_dimmer_button_update_shore(lv_obj_t *btn, const ui_shore_reading_t *r,
                                   bool valid);

/*
 * Feed "is any light on" to a PANEL_BTN_LIGHT_MASTER widget. No-op for any
 * other button type. ui.c computes this by walking the state manager, so the
 * master button stays status-driven like every other button -- it is never
 * flipped by its own tap. Caller must hold the LVGL lock.
 */
void ui_dimmer_button_update_master(lv_obj_t *btn, bool any_light_on);

/*
 * Mark a PANEL_BTN_SCREEN_SWITCH used as a nav-rail entry as the active
 * section, which lights its background. No-op for any other button type,
 * so the plain screen-switch buttons on the non-rail panels are unaffected.
 * Caller must hold the LVGL lock.
 */
void ui_dimmer_button_set_active(lv_obj_t *btn, bool active);

#ifdef __cplusplus
}
#endif
