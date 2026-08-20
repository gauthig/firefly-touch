#include "ui_battery_gauge.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui_theme.h"

typedef struct {
    lv_obj_t *pct_label;
    lv_obj_t *rate_label;
    lv_obj_t *rate_word_label;
    lv_obj_t *eta_label;
    lv_obj_t *popup;
    char      mac[18];
} battery_gauge_ctx_t;

static void popup_click_cb(lv_event_t *e)
{
    lv_obj_t *popup = lv_event_get_target(e);
    lv_obj_add_flag(popup, LV_OBJ_FLAG_HIDDEN);
}

static void container_delete_cb(lv_event_t *e)
{
    battery_gauge_ctx_t *ctx = lv_event_get_user_data(e);
    if (ctx->popup != NULL) {
        lv_obj_delete(ctx->popup);
    }
    lv_free(ctx);
}

lv_obj_t *ui_battery_gauge_create(lv_obj_t *parent)
{
    battery_gauge_ctx_t *ctx = lv_malloc(sizeof(battery_gauge_ctx_t));
    LV_ASSERT_MALLOC(ctx);
    memset(ctx, 0, sizeof(*ctx));
    (void)snprintf(ctx->mac, sizeof(ctx->mac), "--");

    /* Plain box, no border/graphic -- the parent button (ui_dimmer_button.c)
     * already carries the same card background as a light-switch button; this
     * is just a flex column of text inside it. */
    lv_obj_t *wrapper = lv_obj_create(parent);
    lv_obj_set_style_bg_opa(wrapper, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wrapper, 0, 0);
    lv_obj_set_size(wrapper, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(wrapper, 4, 0);
    lv_obj_remove_flag(wrapper, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(wrapper, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(wrapper, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(wrapper, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(wrapper, 6, 0);
    lv_obj_set_user_data(wrapper, ctx);
    lv_obj_add_event_cb(wrapper, container_delete_cb, LV_EVENT_DELETE, ctx);

    ctx->pct_label = lv_label_create(wrapper);
    lv_label_set_text(ctx->pct_label, "--");
    lv_obj_set_style_text_font(ctx->pct_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(ctx->pct_label, UI_COLOR_TEXT, 0);

    ctx->rate_label = lv_label_create(wrapper);
    lv_label_set_text(ctx->rate_label, "--");
    lv_obj_set_style_text_font(ctx->rate_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ctx->rate_label, UI_COLOR_TEXT, 0);

    ctx->rate_word_label = lv_label_create(wrapper);
    lv_label_set_text(ctx->rate_word_label, "");
    lv_obj_set_style_text_font(ctx->rate_word_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(ctx->rate_word_label, UI_COLOR_TEXT_DIM, 0);

    ctx->eta_label = lv_label_create(wrapper);
    lv_label_set_text(ctx->eta_label, "--");
    lv_obj_set_style_text_font(ctx->eta_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(ctx->eta_label, UI_COLOR_TEXT_DIM, 0);

    return wrapper;
}

void ui_battery_gauge_set_status(lv_obj_t *gauge, uint8_t percent, float rate_amps,
                                 float hours, bool valid)
{
    battery_gauge_ctx_t *ctx = lv_obj_get_user_data(gauge);
    if (ctx == NULL) {
        return;
    }

    if (!valid) {
        lv_label_set_text(ctx->pct_label, "--");
        lv_label_set_text(ctx->rate_label, "--");
        lv_label_set_text(ctx->rate_word_label, "");
        lv_label_set_text(ctx->eta_label, "--");
        return;
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "%u%%", (unsigned)(percent > 100 ? 100 : percent));
    lv_label_set_text(ctx->pct_label, buf);

    if (rate_amps > 0.05f) {
        snprintf(buf, sizeof(buf), "%.1fA", (double)rate_amps);
        lv_label_set_text(ctx->rate_label, buf);
        lv_label_set_text(ctx->rate_word_label, "Charging");
    } else if (rate_amps < -0.05f) {
        snprintf(buf, sizeof(buf), "%.1fA", (double)-rate_amps);
        lv_label_set_text(ctx->rate_label, buf);
        lv_label_set_text(ctx->rate_word_label, "Discharging");
    } else {
        lv_label_set_text(ctx->rate_label, "0.0A");
        lv_label_set_text(ctx->rate_word_label, "Idle");
    }

    if (isfinite(hours)) {
        snprintf(buf, sizeof(buf), "%.1fh", (double)hours);
        lv_label_set_text(ctx->eta_label, buf);
    } else {
        lv_label_set_text(ctx->eta_label, "--");
    }
}

void ui_battery_gauge_set_mac(lv_obj_t *gauge, const char *mac)
{
    battery_gauge_ctx_t *ctx = lv_obj_get_user_data(gauge);
    if (ctx == NULL) {
        return;
    }
    if (mac == NULL || mac[0] == '\0') {
        snprintf(ctx->mac, sizeof(ctx->mac), "--");
    } else {
        snprintf(ctx->mac, sizeof(ctx->mac), "%s", mac);
    }
}

void ui_battery_gauge_toggle_mac_popup(lv_obj_t *gauge)
{
    battery_gauge_ctx_t *ctx = lv_obj_get_user_data(gauge);
    if (ctx == NULL) {
        return;
    }

    if (ctx->popup == NULL) {
        lv_obj_t *popup = lv_obj_create(lv_layer_top());
        lv_obj_set_style_bg_color(popup, UI_COLOR_CARD, 0);
        lv_obj_set_style_bg_opa(popup, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(popup, 2, 0);
        lv_obj_set_style_border_color(popup, UI_COLOR_OFF, 0);
        lv_obj_set_style_radius(popup, 10, 0);
        lv_obj_set_style_pad_all(popup, 16, 0);
        lv_obj_set_size(popup, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_remove_flag(popup, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_center(popup);
        lv_obj_add_event_cb(popup, popup_click_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *label = lv_label_create(popup);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(label, UI_COLOR_TEXT, 0);
        lv_label_set_text(label, ctx->mac);

        ctx->popup = popup;
        return;   /* created visible */
    }

    if (lv_obj_has_flag(ctx->popup, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_remove_flag(ctx->popup, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ctx->popup, LV_OBJ_FLAG_HIDDEN);
    }
}
