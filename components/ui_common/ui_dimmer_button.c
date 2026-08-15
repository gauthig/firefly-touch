#include "ui_dimmer_button.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "ui_theme.h"

static const char *TAG = "ui_dimmer_button";

/* RV-C has no command-ack DGN: the only way to know a DC_DIMMER_COMMAND_2
 * landed is to watch for the DC_DIMMER_STATUS_3 that reflects it. Give the
 * bus this long to produce that echo before assuming the command was lost
 * to arbitration and resending once. */
#define CONFIRM_TIMEOUT_MS  800
#define CONFIRM_MAX_RETRIES 1

typedef struct {
    const panel_btn_def_t *def;
    ui_dimmer_send_cb_t    send_cb;
    void                  *user_ctx;

    /* Per-watched-instance state, parallel to def->instances[]. Written
     * only from ui_dimmer_button_update(), i.e. only in response to a real
     * DC_DIMMER_STATUS_3 frame — never optimistically from a local tap. */
    uint8_t levels[PANEL_BTN_MAX_INSTANCES];
    bool    on[PANEL_BTN_MAX_INSTANCES];

    bool ramping;
    bool ramp_up_next;   /* direction for the next hold, alternates */

    /* Command confirmation (tap-to-toggle only). */
    lv_timer_t       *confirm_timer;
    rvc_dimmer_cmd_t   pending_cmd;
    bool               pending_target_on;
    uint8_t            retries_left;

    lv_obj_t *btn;
    lv_obj_t *name;
    lv_obj_t *bar;
    lv_obj_t *value_label;   /* PANEL_BTN_TANK_LEVEL only: "62%" / "--" */
} btn_ctx_t;

static bool any_on(const btn_ctx_t *ctx)
{
    for (uint8_t i = 0; i < ctx->def->instance_count; i++) {
        if (ctx->on[i]) {
            return true;
        }
    }
    return false;
}

static uint8_t max_level(const btn_ctx_t *ctx)
{
    uint8_t max = 0;
    for (uint8_t i = 0; i < ctx->def->instance_count; i++) {
        if (ctx->levels[i] > max) {
            max = ctx->levels[i];
        }
    }
    return max;
}

static void refresh_visuals(btn_ctx_t *ctx)
{
    const bool on = any_on(ctx);
    lv_obj_set_style_bg_color(ctx->btn, on ? UI_COLOR_CARD_ON : UI_COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_text_color(ctx->name, on ? UI_COLOR_TEXT_ON_LIT : UI_COLOR_TEXT_DIM, 0);

    if (ctx->bar != NULL) {
        if (ctx->def->type == PANEL_BTN_TANK_LEVEL) {
            /* Always visible -- a tank reading isn't an on/off state. */
            lv_obj_remove_flag(ctx->bar, LV_OBJ_FLAG_HIDDEN);
        } else if (on) {
            lv_obj_remove_flag(ctx->bar, LV_OBJ_FLAG_HIDDEN);
            lv_bar_set_value(ctx->bar, max_level(ctx), LV_ANIM_OFF);
        } else {
            lv_obj_add_flag(ctx->bar, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void send(btn_ctx_t *ctx, rvc_dimmer_cmd_t cmd)
{
    if (ctx->send_cb != NULL) {
        ctx->send_cb(ctx->def, cmd, ctx->user_ctx);
    }
}

static void confirm_timer_cb(lv_timer_t *t)
{
    btn_ctx_t *ctx = lv_timer_get_user_data(t);
    ctx->confirm_timer = NULL;

    if (any_on(ctx) == ctx->pending_target_on) {
        /* STATUS_3 already confirmed it via ui_dimmer_button_update();
         * the timer just lost the race to being deleted. Nothing to do. */
        return;
    }

    if (ctx->retries_left == 0) {
        ESP_LOGW(TAG, "%s: no STATUS_3 confirming target state, giving up",
                 ctx->def->label);
        return;
    }

    ESP_LOGW(TAG, "%s: no STATUS_3 within %d ms, resending cmd %d",
             ctx->def->label, CONFIRM_TIMEOUT_MS, (int)ctx->pending_cmd);
    ctx->retries_left--;
    send(ctx, ctx->pending_cmd);
    ctx->confirm_timer = lv_timer_create(confirm_timer_cb, CONFIRM_TIMEOUT_MS, ctx);
    lv_timer_set_repeat_count(ctx->confirm_timer, 1);
}

static void handle_tap(btn_ctx_t *ctx)
{
    if (ctx->def->type == PANEL_BTN_SCREEN_SWITCH) {
        /* Local UI nav, not an RV-C command -- the receiver (ui.c's
         * panel_send_cb) switches screens and never forwards this to the
         * bus. The command value itself is a don't-care signal. */
        send(ctx, RVC_DIMMER_CMD_TOGGLE);
        return;
    }
    if (ctx->def->type == PANEL_BTN_TANK_LEVEL) {
        /* Read-only display, no command, no confirm timer. */
        return;
    }

    /* Use explicit ON/OFF rather than TOGGLE for all button types. TOGGLE
     * is unreliable when a DC_DIMMER_STATUS_3 frame has been missed: the
     * panel's tracked on/off then disagrees with the real load, and TOGGLE
     * fires in the wrong direction. ON/OFF is idempotent.
     *
     * RV-C has no command-ack DGN, so "did it work" can only be answered
     * by watching for the STATUS_3 the G6 broadcasts once the load
     * actually changes. Send once, arm a confirm timer, and resend (once)
     * only if that echo doesn't show up in time. The visual state itself
     * is never touched here — it only ever moves in
     * ui_dimmer_button_update(), driven by a real status frame, per the
     * project's status-driven-UI invariant. */
    const bool currently_on = any_on(ctx);
    const rvc_dimmer_cmd_t cmd = currently_on ? RVC_DIMMER_CMD_OFF
                                              : RVC_DIMMER_CMD_ON_DELAY;
    send(ctx, cmd);

    if (ctx->confirm_timer != NULL) {
        lv_timer_delete(ctx->confirm_timer);
    }
    ctx->pending_cmd = cmd;
    ctx->pending_target_on = !currently_on;
    ctx->retries_left = CONFIRM_MAX_RETRIES;
    ctx->confirm_timer = lv_timer_create(confirm_timer_cb, CONFIRM_TIMEOUT_MS, ctx);
    lv_timer_set_repeat_count(ctx->confirm_timer, 1);
}

static void event_cb(lv_event_t *e)
{
    btn_ctx_t *ctx = lv_event_get_user_data(e);
    const lv_event_code_t code = lv_event_get_code(e);

    switch (code) {
    case LV_EVENT_SHORT_CLICKED:
        handle_tap(ctx);
        break;

    case LV_EVENT_LONG_PRESSED:
        if (ctx->def->type == PANEL_BTN_DIMMER) {
            ctx->ramping = true;
            if (!any_on(ctx)) {
                ctx->ramp_up_next = true;
            }
            send(ctx, ctx->ramp_up_next ? RVC_DIMMER_CMD_RAMP_UP
                                        : RVC_DIMMER_CMD_RAMP_DOWN);
        }
        break;

    case LV_EVENT_LONG_PRESSED_REPEAT:
        /* Keep the ramp alive while held (~every 100 ms; harmless at
         * 250 kbps, and covers controllers that time ramps out). */
        if (ctx->ramping) {
            send(ctx, ctx->ramp_up_next ? RVC_DIMMER_CMD_RAMP_UP
                                        : RVC_DIMMER_CMD_RAMP_DOWN);
        }
        break;

    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        if (ctx->ramping) {
            ctx->ramping = false;
            ctx->ramp_up_next = !ctx->ramp_up_next;
            send(ctx, RVC_DIMMER_CMD_STOP);
        }
        break;

    case LV_EVENT_DELETE:
        if (ctx->confirm_timer != NULL) {
            lv_timer_delete(ctx->confirm_timer);
        }
        lv_free(ctx);
        break;

    default:
        break;
    }
}

lv_obj_t *ui_dimmer_button_create(lv_obj_t *parent,
                                  const panel_btn_def_t *def,
                                  ui_dimmer_send_cb_t send_cb,
                                  void *user_ctx)
{
    btn_ctx_t *ctx = lv_malloc(sizeof(btn_ctx_t));
    LV_ASSERT_MALLOC(ctx);
    memset(ctx, 0, sizeof(*ctx));
    ctx->def = def;
    ctx->send_cb = send_cb;
    ctx->user_ctx = user_ctx;
    ctx->ramp_up_next = true;

    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_add_style(btn, &ui_style_card, LV_STATE_DEFAULT);
    lv_obj_add_style(btn, &ui_style_card_pressed, LV_STATE_PRESSED);
    lv_obj_set_user_data(btn, ctx);
    ctx->btn = btn;
    /* Keep LV_OBJ_FLAG_PRESS_LOCK (the LVGL default) so that a small
     * finger drift cannot fire PRESS_LOST before LONG_PRESSED.
     * Without it, the 400 ms long-press timer resets whenever the touch
     * position moves even one pixel, which makes hold-to-ramp unreliable
     * on a capacitive panel. */

    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(btn, 2, 0);

    ctx->name = lv_label_create(btn);
    lv_label_set_text(ctx->name, def->label);
    lv_obj_set_style_text_font(ctx->name, &lv_font_montserrat_20, 0);

    if (def->type == PANEL_BTN_DIMMER || def->type == PANEL_BTN_TANK_LEVEL) {
        ctx->bar = lv_bar_create(btn);
        lv_obj_set_size(ctx->bar, 90, 5);
        lv_bar_set_range(ctx->bar, 0, def->type == PANEL_BTN_TANK_LEVEL ? 100 : RVC_LEVEL_MAX);
        lv_obj_set_style_bg_color(ctx->bar, UI_COLOR_OFF, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(ctx->bar, LV_OPA_40, LV_PART_MAIN);
        lv_obj_set_style_bg_color(ctx->bar, UI_COLOR_AMBER, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(ctx->bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_add_flag(ctx->bar, LV_OBJ_FLAG_HIDDEN);
    }

    if (def->type == PANEL_BTN_TANK_LEVEL) {
        ctx->value_label = lv_label_create(btn);
        lv_label_set_text(ctx->value_label, "--");
        lv_obj_set_style_text_font(ctx->value_label, &lv_font_montserrat_20, 0);
    }

    lv_obj_add_event_cb(btn, event_cb, LV_EVENT_ALL, ctx);
    refresh_visuals(ctx);
    return btn;
}

void ui_dimmer_button_update(lv_obj_t *btn, uint8_t instance,
                             uint8_t level, bool on)
{
    btn_ctx_t *ctx = lv_obj_get_user_data(btn);
    /* Tank widgets never participate in this pathway -- a numeric instance
     * collision with a real dimmer (e.g. both using instance 1) must not
     * cross-contaminate a tank-level button's state. See
     * ui_dimmer_button_update_tank() for the tank-only equivalent. */
    if (ctx == NULL || ctx->def->type == PANEL_BTN_TANK_LEVEL) {
        return;
    }

    bool hit = false;
    for (uint8_t i = 0; i < ctx->def->instance_count; i++) {
        if (ctx->def->instances[i] == instance) {
            ctx->levels[i] = level;
            ctx->on[i] = on;
            hit = true;
        }
    }
    if (hit) {
        refresh_visuals(ctx);
        if (ctx->confirm_timer != NULL && any_on(ctx) == ctx->pending_target_on) {
            lv_timer_delete(ctx->confirm_timer);
            ctx->confirm_timer = NULL;
        }
    }
}

void ui_dimmer_button_update_tank(lv_obj_t *btn, uint8_t instance,
                                  uint8_t percent, bool valid)
{
    btn_ctx_t *ctx = lv_obj_get_user_data(btn);
    if (ctx == NULL || ctx->def->type != PANEL_BTN_TANK_LEVEL) {
        return;
    }

    bool hit = false;
    for (uint8_t i = 0; i < ctx->def->instance_count; i++) {
        if (ctx->def->instances[i] == instance) {
            hit = true;
        }
    }
    if (!hit) {
        return;
    }

    if (!valid) {
        lv_label_set_text(ctx->value_label, "--");
        lv_bar_set_value(ctx->bar, 0, LV_ANIM_OFF);
        return;
    }

    char buf[8];
    snprintf(buf, sizeof(buf), "%u%%", (unsigned)percent);
    lv_label_set_text(ctx->value_label, buf);
    lv_bar_set_value(ctx->bar, percent, LV_ANIM_OFF);
}
