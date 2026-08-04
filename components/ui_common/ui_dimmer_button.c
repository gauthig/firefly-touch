#include "ui_dimmer_button.h"

#include <string.h>

#include "ui_theme.h"

typedef struct {
    const panel_btn_def_t *def;
    ui_dimmer_send_cb_t    send_cb;
    void                  *user_ctx;

    /* Per-watched-instance state, parallel to def->instances[]. */
    uint8_t levels[PANEL_BTN_MAX_INSTANCES];
    bool    on[PANEL_BTN_MAX_INSTANCES];

    bool ramping;
    bool ramp_up_next;   /* direction for the next hold, alternates */

    lv_obj_t *icon;
    lv_obj_t *name;
    lv_obj_t *bar;
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
    lv_obj_set_style_text_color(ctx->icon, on ? UI_COLOR_AMBER : UI_COLOR_OFF, 0);
    lv_obj_set_style_text_color(ctx->name, on ? UI_COLOR_TEXT : UI_COLOR_TEXT_DIM, 0);

    if (ctx->bar != NULL) {
        if (on) {
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

static void handle_tap(btn_ctx_t *ctx)
{
    const panel_btn_def_t *def = ctx->def;

    if (def->type == PANEL_BTN_PANEL_LIGHTS) {
        /* Placeholder load — the callback owns what "press" means. */
        send(ctx, RVC_DIMMER_CMD_TOGGLE);
        return;
    }

    if (def->instance_count == 1 && def->type == PANEL_BTN_DIMMER) {
        send(ctx, RVC_DIMMER_CMD_TOGGLE);
    } else {
        /* Grouped loads / switches: same explicit command to every
         * instance so members can't end up out of phase. */
        send(ctx, any_on(ctx) ? RVC_DIMMER_CMD_OFF : RVC_DIMMER_CMD_ON_DELAY);
    }
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
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_PRESS_LOCK);

    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(btn, 2, 0);

    ctx->icon = lv_label_create(btn);
    lv_label_set_text(ctx->icon, def->symbol);
    lv_obj_set_style_text_font(ctx->icon, &lv_font_montserrat_28, 0);

    ctx->name = lv_label_create(btn);
    lv_label_set_text(ctx->name, def->label);
    lv_obj_set_style_text_font(ctx->name, &lv_font_montserrat_16, 0);

    if (def->type == PANEL_BTN_DIMMER) {
        ctx->bar = lv_bar_create(btn);
        lv_obj_set_size(ctx->bar, 90, 5);
        lv_bar_set_range(ctx->bar, 0, RVC_LEVEL_MAX);
        lv_obj_set_style_bg_color(ctx->bar, UI_COLOR_OFF, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(ctx->bar, LV_OPA_40, LV_PART_MAIN);
        lv_obj_set_style_bg_color(ctx->bar, UI_COLOR_AMBER, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(ctx->bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_add_flag(ctx->bar, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_add_event_cb(btn, event_cb, LV_EVENT_ALL, ctx);
    refresh_visuals(ctx);
    return btn;
}

void ui_dimmer_button_update(lv_obj_t *btn, uint8_t instance,
                             uint8_t level, bool on)
{
    btn_ctx_t *ctx = lv_obj_get_user_data(btn);
    if (ctx == NULL) {
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
    }
}
