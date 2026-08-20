#include "ui_battery_gauge.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui_theme.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define WAVE_ANIM_PERIOD_MS 100
#define WAVE_AMPLITUDE_PX   3
#define WAVE_POINT_STEP_PX  5
#define WAVE_PHASE_STEP     0.35f

#define SOC_BAND_WARN_PCT 50   /* below this: warn color */
#define SOC_BAND_ERR_PCT  20   /* below this: err color */

typedef struct {
    lv_obj_t *water;   /* draws the fill + wave surface, inside the body */
    lv_obj_t *pct_label;
    lv_obj_t *rate_label;
    lv_obj_t *eta_label;
    lv_timer_t *anim_timer;
    uint8_t   percent;
    bool      valid;
    float     phase;
} battery_gauge_ctx_t;

static lv_color_t band_color(uint8_t percent)
{
    if (percent < SOC_BAND_ERR_PCT) {
        return UI_COLOR_ERR;
    }
    if (percent < SOC_BAND_WARN_PCT) {
        return UI_COLOR_WARN;
    }
    return UI_COLOR_OK;
}

static void water_draw_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN) {
        return;
    }

    lv_obj_t *obj = lv_event_get_target(e);
    battery_gauge_ctx_t *ctx = lv_event_get_user_data(e);
    if (!ctx->valid || ctx->percent == 0) {
        return;
    }

    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t area;
    lv_obj_get_coords(obj, &area);

    const int32_t w = lv_area_get_width(&area);
    const int32_t h = lv_area_get_height(&area);
    const uint8_t pct = ctx->percent > 100 ? 100 : ctx->percent;
    const int32_t fill_h = (h * pct) / 100;
    const int32_t wave_y = area.y2 - fill_h;
    const lv_color_t color = band_color(pct);

    lv_draw_rect_dsc_t rect_dsc;
    lv_draw_rect_dsc_init(&rect_dsc);
    rect_dsc.bg_color = color;
    rect_dsc.bg_opa = LV_OPA_COVER;
    lv_area_t fill_area = { area.x1, wave_y, area.x2, area.y2 };
    lv_draw_rect(layer, &rect_dsc, &fill_area);

    lv_point_precise_t points[24];
    int32_t point_cnt = (w / WAVE_POINT_STEP_PX) + 2;
    if (point_cnt > (int32_t)(sizeof(points) / sizeof(points[0]))) {
        point_cnt = sizeof(points) / sizeof(points[0]);
    }
    for (int32_t i = 0; i < point_cnt; i++) {
        int32_t x = area.x1 + (i * w) / (point_cnt - 1);
        float y_off = sinf(ctx->phase + (float)i * 0.9f) * WAVE_AMPLITUDE_PX;
        points[i].x = x;
        points[i].y = wave_y + (int32_t)y_off;
    }

    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = color;
    line_dsc.width = 2;
    line_dsc.opa = LV_OPA_COVER;
    line_dsc.round_start = 1;
    line_dsc.round_end = 1;
    line_dsc.points = points;
    line_dsc.point_cnt = point_cnt;
    lv_draw_line(layer, &line_dsc);
}

static void anim_timer_cb(lv_timer_t *t)
{
    battery_gauge_ctx_t *ctx = lv_timer_get_user_data(t);
    ctx->phase += WAVE_PHASE_STEP;
    if (ctx->phase > 2.0f * (float)M_PI) {
        ctx->phase -= 2.0f * (float)M_PI;
    }
    if (ctx->valid && ctx->percent > 0) {
        lv_obj_invalidate(ctx->water);
    }
}

static void container_delete_cb(lv_event_t *e)
{
    battery_gauge_ctx_t *ctx = lv_event_get_user_data(e);
    if (ctx->anim_timer != NULL) {
        lv_timer_delete(ctx->anim_timer);
    }
    lv_free(ctx);
}

lv_obj_t *ui_battery_gauge_create(lv_obj_t *parent)
{
    battery_gauge_ctx_t *ctx = lv_malloc(sizeof(battery_gauge_ctx_t));
    LV_ASSERT_MALLOC(ctx);
    memset(ctx, 0, sizeof(*ctx));

    lv_obj_t *wrapper = lv_obj_create(parent);
    lv_obj_add_style(wrapper, &ui_style_screen, 0);
    lv_obj_set_size(wrapper, 130, LV_PCT(100));
    lv_obj_set_style_pad_all(wrapper, 0, 0);
    lv_obj_remove_flag(wrapper, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(wrapper, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(wrapper, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(wrapper, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(wrapper, 4, 0);
    lv_obj_set_user_data(wrapper, ctx);
    lv_obj_add_event_cb(wrapper, container_delete_cb, LV_EVENT_DELETE, ctx);

    /* Nub -- the small top terminal of the battery silhouette. */
    lv_obj_t *nub = lv_obj_create(wrapper);
    lv_obj_set_size(nub, 28, 8);
    lv_obj_set_style_bg_color(nub, UI_COLOR_OFF, 0);
    lv_obj_set_style_bg_opa(nub, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(nub, 0, 0);
    lv_obj_set_style_radius(nub, 2, 0);
    lv_obj_set_style_pad_all(nub, 0, 0);
    lv_obj_remove_flag(nub, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(nub, LV_OBJ_FLAG_CLICKABLE);

    /* Body -- the "glass" that gets filled. */
    lv_obj_t *body = lv_obj_create(wrapper);
    lv_obj_set_size(body, 84, 96);
    lv_obj_set_style_bg_color(body, UI_COLOR_TANK_EMPTY, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(body, 2, 0);
    lv_obj_set_style_border_color(body, UI_COLOR_OFF, 0);
    lv_obj_set_style_radius(body, 8, 0);
    lv_obj_set_style_clip_corner(body, true, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *water = lv_obj_create(body);
    lv_obj_set_size(water, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(water, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(water, 0, 0);
    lv_obj_set_style_radius(water, 0, 0);
    lv_obj_set_style_pad_all(water, 0, 0);
    lv_obj_remove_flag(water, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(water, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(water, water_draw_event_cb, LV_EVENT_DRAW_MAIN, ctx);
    ctx->water = water;

    lv_obj_t *pct_label = lv_label_create(body);
    lv_label_set_text(pct_label, "--");
    lv_obj_set_style_text_font(pct_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(pct_label, UI_COLOR_TEXT, 0);
    lv_obj_align(pct_label, LV_ALIGN_TOP_MID, 0, 4);
    ctx->pct_label = pct_label;

    ctx->rate_label = lv_label_create(wrapper);
    lv_label_set_text(ctx->rate_label, "--");
    lv_obj_set_style_text_font(ctx->rate_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(ctx->rate_label, UI_COLOR_TEXT, 0);

    ctx->eta_label = lv_label_create(wrapper);
    lv_label_set_text(ctx->eta_label, "--");
    lv_obj_set_style_text_font(ctx->eta_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ctx->eta_label, UI_COLOR_TEXT_DIM, 0);

    ctx->anim_timer = lv_timer_create(anim_timer_cb, WAVE_ANIM_PERIOD_MS, ctx);

    return wrapper;
}

void ui_battery_gauge_set_status(lv_obj_t *gauge, uint8_t percent, float rate_amps,
                                 float hours, bool valid)
{
    battery_gauge_ctx_t *ctx = lv_obj_get_user_data(gauge);
    if (ctx == NULL) {
        return;
    }

    ctx->valid = valid;
    ctx->percent = percent;

    if (!valid) {
        lv_label_set_text(ctx->pct_label, "--");
        lv_label_set_text(ctx->rate_label, "--");
        lv_label_set_text(ctx->eta_label, "--");
        lv_obj_invalidate(ctx->water);
        return;
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "%u%%", (unsigned)(percent > 100 ? 100 : percent));
    lv_label_set_text(ctx->pct_label, buf);

    if (rate_amps > 0.05f) {
        snprintf(buf, sizeof(buf), "%.1fA chg", (double)rate_amps);
    } else if (rate_amps < -0.05f) {
        snprintf(buf, sizeof(buf), "%.1fA dis", (double)-rate_amps);
    } else {
        snprintf(buf, sizeof(buf), "0.0A idle");
    }
    lv_label_set_text(ctx->rate_label, buf);

    if (isfinite(hours)) {
        snprintf(buf, sizeof(buf), "%.1fh", (double)hours);
        lv_label_set_text(ctx->eta_label, buf);
    } else {
        lv_label_set_text(ctx->eta_label, "--");
    }

    lv_obj_invalidate(ctx->water);
}
