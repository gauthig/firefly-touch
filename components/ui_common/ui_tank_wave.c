#include "ui_tank_wave.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui_theme.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define WAVE_ANIM_PERIOD_MS 100
#define WAVE_AMPLITUDE_PX   4
#define WAVE_POINT_STEP_PX  6
#define WAVE_PHASE_STEP     0.35f /* radians advanced per animation tick */

typedef struct {
    lv_obj_t *water;   /* draws the fill + wave surface */
    lv_obj_t *label;   /* "62%" / "--" */
    lv_timer_t *anim_timer;
    uint8_t   percent;
    bool      valid;
    float     phase;
} tank_wave_ctx_t;

static void water_draw_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN) {
        return;
    }

    lv_obj_t *obj = lv_event_get_target(e);
    tank_wave_ctx_t *ctx = lv_event_get_user_data(e);
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

    /* Flat body of water from the wave line down to the tank bottom. */
    lv_draw_rect_dsc_t rect_dsc;
    lv_draw_rect_dsc_init(&rect_dsc);
    rect_dsc.bg_color = UI_COLOR_CARD_ON;
    rect_dsc.bg_opa = LV_OPA_COVER;
    lv_area_t fill_area = { area.x1, wave_y, area.x2, area.y2 };
    lv_draw_rect(layer, &rect_dsc, &fill_area);

    /* Sine-wave surface riding the fill line, phase-animated. */
    lv_point_precise_t points[32];
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
    line_dsc.color = UI_COLOR_CARD_ON;
    line_dsc.width = 3;
    line_dsc.opa = LV_OPA_COVER;
    line_dsc.round_start = 1;
    line_dsc.round_end = 1;
    line_dsc.points = points;
    line_dsc.point_cnt = point_cnt;
    lv_draw_line(layer, &line_dsc);
}

static void anim_timer_cb(lv_timer_t *t)
{
    tank_wave_ctx_t *ctx = lv_timer_get_user_data(t);
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
    tank_wave_ctx_t *ctx = lv_event_get_user_data(e);
    if (ctx->anim_timer != NULL) {
        lv_timer_delete(ctx->anim_timer);
    }
    lv_free(ctx);
}

lv_obj_t *ui_tank_wave_create(lv_obj_t *parent)
{
    tank_wave_ctx_t *ctx = lv_malloc(sizeof(tank_wave_ctx_t));
    LV_ASSERT_MALLOC(ctx);
    memset(ctx, 0, sizeof(*ctx));

    lv_obj_t *glass = lv_obj_create(parent);
    lv_obj_set_size(glass, 90, 90);
    lv_obj_set_style_bg_color(glass, UI_COLOR_TANK_EMPTY, 0);
    lv_obj_set_style_bg_opa(glass, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(glass, 2, 0);
    lv_obj_set_style_border_color(glass, UI_COLOR_OFF, 0);
    lv_obj_set_style_radius(glass, 12, 0);
    lv_obj_set_style_clip_corner(glass, true, 0);
    lv_obj_set_style_pad_all(glass, 0, 0);
    lv_obj_remove_flag(glass, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(glass, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(glass, ctx);
    lv_obj_add_event_cb(glass, container_delete_cb, LV_EVENT_DELETE, ctx);

    lv_obj_t *water = lv_obj_create(glass);
    lv_obj_set_size(water, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(water, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(water, 0, 0);
    lv_obj_set_style_radius(water, 0, 0);
    lv_obj_set_style_pad_all(water, 0, 0);
    lv_obj_remove_flag(water, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(water, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(water, water_draw_event_cb, LV_EVENT_DRAW_MAIN, ctx);
    ctx->water = water;

    lv_obj_t *label = lv_label_create(glass);
    lv_label_set_text(label, "--");
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(label, UI_COLOR_TEXT, 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 4);
    ctx->label = label;

    ctx->anim_timer = lv_timer_create(anim_timer_cb, WAVE_ANIM_PERIOD_MS, ctx);

    return glass;
}

void ui_tank_wave_set_percent(lv_obj_t *wave, uint8_t percent, bool valid)
{
    tank_wave_ctx_t *ctx = lv_obj_get_user_data(wave);
    if (ctx == NULL) {
        return;
    }

    ctx->valid = valid;
    ctx->percent = percent;

    if (!valid) {
        lv_label_set_text(ctx->label, "--");
    } else {
        char buf[8];
        snprintf(buf, sizeof(buf), "%u%%", (unsigned)percent);
        lv_label_set_text(ctx->label, buf);
    }
    lv_obj_invalidate(ctx->water);
}
