#include "ui_solar_panel.h"

#include <stdio.h>
#include <string.h>

#include "renogy_solar_protocol.h"
#include "ui_theme.h"

/* Reading order: the solar side on the top row, the battery/thermal side
 * beneath it. Watts leads because it is the number anyone actually looks
 * for -- "is it making power right now". */
typedef enum {
    CELL_WATTS = 0,
    CELL_PV_VOLTS,
    CELL_PV_AMPS,
    CELL_BATT_VOLTS,
    CELL_CTRL_TEMP,
    CELL_BATT_TEMP,
    CELL_COUNT,
} cell_id_t;

static const char *const k_caption[CELL_COUNT] = {
    "Watts", "PV Volts", "PV Amps", "Batt Volts", "Ctrl Temp", "Batt Temp",
};
/*
 * "F", not "°F": LVGL's built-in Montserrat faces only carry ASCII, so a
 * degree sign renders as a missing-glyph box. The rest of the project
 * already writes plain F for the same reason (see ui_battery_summary).
 */
static const char *const k_unit[CELL_COUNT] = { "W", "V", "A", "V", "F", "F" };

#define GRID_COLS 3

typedef struct {
    lv_obj_t *state_label;
    lv_obj_t *value[CELL_COUNT];
} solar_ctx_t;

static void panel_delete_cb(lv_event_t *e)
{
    lv_free(lv_event_get_user_data(e));
}

/*
 * Colour for the charging state. Green only when the controller is actually
 * pushing charge; a bare wattage cannot distinguish "0 W because it is dark"
 * from "0 W because the battery is full", and this is what does.
 */
static lv_color_t state_color(uint8_t state)
{
    switch (state) {
    case RENOGY_CHARGE_MPPT:
    case RENOGY_CHARGE_BOOST:
    case RENOGY_CHARGE_FLOATING:
    case RENOGY_CHARGE_EQUALIZING:
    case RENOGY_CHARGE_ACTIVATED:
        return UI_COLOR_OK;
    case RENOGY_CHARGE_CURRENT_LIM:
        return UI_COLOR_WARN;
    default:
        return UI_COLOR_TEXT_DIM;
    }
}

/* One "Watts / 74 / W" tile, matching ui_shore_panel's so the two readouts
 * look like the same family of thing. */
static lv_obj_t *make_tile(lv_obj_t *parent, uint8_t col, uint8_t row,
                           const char *caption, const char *unit)
{
    lv_obj_t *tile = lv_obj_create(parent);
    lv_obj_set_style_bg_color(tile, UI_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tile, 0, 0);
    lv_obj_set_style_radius(tile, 8, 0);
    lv_obj_set_style_pad_all(tile, 4, 0);
    lv_obj_set_style_pad_row(tile, 0, 0);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_grid_cell(tile, LV_GRID_ALIGN_STRETCH, col, 1,
                         LV_GRID_ALIGN_STRETCH, row, 1);

    lv_obj_t *cap = lv_label_create(tile);
    lv_label_set_text(cap, caption);
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cap, UI_COLOR_TEXT_DIM, 0);

    lv_obj_t *val = lv_label_create(tile);
    lv_label_set_text(val, "--");
    lv_obj_set_style_text_font(val, &lv_font_montserrat_28, 0);
    /* Green digits, same as the shore readout: this is a measurement, not a
     * control, and the colour keeps it distinct from the light buttons. */
    lv_obj_set_style_text_color(val, UI_COLOR_OK, 0);

    lv_obj_t *u = lv_label_create(tile);
    lv_label_set_text(u, unit);
    lv_obj_set_style_text_font(u, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(u, UI_COLOR_TEXT_DIM, 0);

    return val;
}

lv_obj_t *ui_solar_panel_create(lv_obj_t *parent)
{
    solar_ctx_t *ctx = lv_malloc(sizeof(solar_ctx_t));
    LV_ASSERT_MALLOC(ctx);
    memset(ctx, 0, sizeof(*ctx));

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 4, 0);
    lv_obj_set_style_pad_row(root, 4, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_user_data(root, ctx);
    lv_obj_add_event_cb(root, panel_delete_cb, LV_EVENT_DELETE, ctx);

    /* --- header: section name on the left, charging state on the right --- */
    lv_obj_t *header = lv_obj_create(root);
    lv_obj_set_size(header, LV_PCT(100), 26);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "SOLAR");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, UI_COLOR_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 2, 0);

    ctx->state_label = lv_label_create(header);
    lv_label_set_text(ctx->state_label, "--");
    lv_obj_set_style_text_font(ctx->state_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ctx->state_label, UI_COLOR_TEXT_DIM, 0);
    lv_obj_align(ctx->state_label, LV_ALIGN_RIGHT_MID, -2, 0);

    /* --- 3x2 tile grid, filling whatever height is left --- */
    static int32_t col_dsc[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
                                 LV_GRID_TEMPLATE_LAST };
    static int32_t row_dsc[] = { LV_GRID_FR(1), LV_GRID_FR(1),
                                 LV_GRID_TEMPLATE_LAST };

    lv_obj_t *grid = lv_obj_create(root);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_flex_grow(grid, 1);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_style_pad_row(grid, 6, 0);
    lv_obj_set_style_pad_column(grid, 6, 0);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_grid_dsc_array(grid, col_dsc, row_dsc);

    for (uint8_t c = 0; c < CELL_COUNT; c++) {
        ctx->value[c] = make_tile(grid, c % GRID_COLS, c / GRID_COLS,
                                  k_caption[c], k_unit[c]);
    }

    return root;
}

static void set_all_dashes(solar_ctx_t *ctx)
{
    for (uint8_t c = 0; c < CELL_COUNT; c++) {
        lv_label_set_text(ctx->value[c], "--");
        lv_obj_set_style_text_color(ctx->value[c], UI_COLOR_TEXT_DIM, 0);
    }
}

void ui_solar_panel_set(lv_obj_t *panel, const ui_solar_reading_t *r, bool valid)
{
    solar_ctx_t *ctx = lv_obj_get_user_data(panel);
    if (ctx == NULL) {
        return;
    }

    if (!valid || r == NULL) {
        set_all_dashes(ctx);
        lv_label_set_text(ctx->state_label, "offline");
        lv_obj_set_style_text_color(ctx->state_label, UI_COLOR_ERR, 0);
        return;
    }

    char buf[16];

    snprintf(buf, sizeof(buf), "%.0f", (double)r->pv_watts);
    lv_label_set_text(ctx->value[CELL_WATTS], buf);

    snprintf(buf, sizeof(buf), "%.1f", (double)r->pv_volts);
    lv_label_set_text(ctx->value[CELL_PV_VOLTS], buf);

    /* Two decimals on PV amps: the controller reports centiamps, and near
     * dawn/dusk the whole reading lives below 1 A, where %.1f would round
     * a real trickle to a flat 0.0. */
    snprintf(buf, sizeof(buf), "%.2f", (double)r->pv_amps);
    lv_label_set_text(ctx->value[CELL_PV_AMPS], buf);

    snprintf(buf, sizeof(buf), "%.1f", (double)r->battery_volts);
    lv_label_set_text(ctx->value[CELL_BATT_VOLTS], buf);

    for (uint8_t c = 0; c <= CELL_BATT_VOLTS; c++) {
        lv_obj_set_style_text_color(ctx->value[c], UI_COLOR_OK, 0);
    }

    if (r->temp_valid) {
        snprintf(buf, sizeof(buf), "%.0f", (double)r->controller_temp_f);
        lv_label_set_text(ctx->value[CELL_CTRL_TEMP], buf);
        snprintf(buf, sizeof(buf), "%.0f", (double)r->battery_temp_f);
        lv_label_set_text(ctx->value[CELL_BATT_TEMP], buf);
        lv_obj_set_style_text_color(ctx->value[CELL_CTRL_TEMP], UI_COLOR_OK, 0);
        lv_obj_set_style_text_color(ctx->value[CELL_BATT_TEMP], UI_COLOR_OK, 0);
    } else {
        lv_label_set_text(ctx->value[CELL_CTRL_TEMP], "--");
        lv_label_set_text(ctx->value[CELL_BATT_TEMP], "--");
        lv_obj_set_style_text_color(ctx->value[CELL_CTRL_TEMP], UI_COLOR_TEXT_DIM, 0);
        lv_obj_set_style_text_color(ctx->value[CELL_BATT_TEMP], UI_COLOR_TEXT_DIM, 0);
    }

    lv_label_set_text(ctx->state_label, renogy_charge_state_str(r->charge_state));
    lv_obj_set_style_text_color(ctx->state_label, state_color(r->charge_state), 0);
}
