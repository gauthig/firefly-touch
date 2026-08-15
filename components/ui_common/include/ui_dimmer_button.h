/*
 * ui_dimmer_button — shared button widget for dimmer / switch / placeholder
 * loads.
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
 * Visual state is driven ONLY through ui_dimmer_button_update() — i.e. only
 * by DC_DIMMER_STATUS_3 frames from the bus. Sending a command never changes
 * the widget locally, so this panel stays consistent with factory switches
 * and the Firefly app.
 */
#pragma once

#include "lvgl.h"
#include "panel_def.h"
#include "rvc_protocol.h"

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
 * that instance the call is a no-op. Shows ON if any watched instance is on;
 * brightness bar shows the highest reported level. Caller must hold the LVGL
 * lock.
 */
void ui_dimmer_button_update(lv_obj_t *btn, uint8_t instance,
                             uint8_t level, bool on);

#ifdef __cplusplus
}
#endif
