#include "ui_shore_panel.h"

#include <stdio.h>
#include <string.h>

#include "ui_theme.h"

/* Four readouts per line, in the same order the Hughes app uses. */
typedef enum {
    CELL_VOLTS = 0,
    CELL_AMPS,
    CELL_FREQ,
    CELL_WATTS,
    CELL_COUNT,
} cell_id_t;

static const char *const k_cell_caption[CELL_COUNT] = { "Volts", "Amps", "Freq", "Watts" };
static const char *const k_cell_unit[CELL_COUNT]    = { "V", "A", "Hz", "W" };

typedef struct {
    lv_obj_t *column[2];               /* whole column, hidden when absent */
    lv_obj_t *value[2][CELL_COUNT];
} shore_ctx_t;

static void panel_delete_cb(lv_event_t *e)
{
    lv_free(lv_event_get_user_data(e));
}

/* One "Volts / 118 / V" tile. */
static lv_obj_t *make_tile(lv_obj_t *parent, const char *caption, const char *unit)
{
    lv_obj_t *tile = lv_obj_create(parent);
    lv_obj_set_width(tile, LV_PCT(100));
    lv_obj_set_flex_grow(tile, 1);
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

    lv_obj_t *cap = lv_label_create(tile);
    lv_label_set_text(cap, caption);
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cap, UI_COLOR_TEXT_DIM, 0);

    lv_obj_t *val = lv_label_create(tile);
    lv_label_set_text(val, "--");
    lv_obj_set_style_text_font(val, &lv_font_montserrat_28, 0);
    /* Green digits, echoing the vendor app's segment display -- this is a
     * readout, not a control, and the colour keeps it visually distinct
     * from the light-switch buttons. */
    lv_obj_set_style_text_color(val, UI_COLOR_OK, 0);

    lv_obj_t *u = lv_label_create(tile);
    lv_label_set_text(u, unit);
    lv_obj_set_style_text_font(u, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(u, UI_COLOR_TEXT_DIM, 0);

    return val;
}

lv_obj_t *ui_shore_panel_create(lv_obj_t *parent)
{
    shore_ctx_t *ctx = lv_malloc(sizeof(shore_ctx_t));
    LV_ASSERT_MALLOC(ctx);
    memset(ctx, 0, sizeof(*ctx));

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 4, 0);
    lv_obj_set_style_pad_column(root, 8, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_user_data(root, ctx);
    lv_obj_add_event_cb(root, panel_delete_cb, LV_EVENT_DELETE, ctx);

    for (uint8_t line = 0; line < 2; line++) {
        lv_obj_t *col = lv_obj_create(root);
        lv_obj_set_height(col, LV_PCT(100));
        lv_obj_set_flex_grow(col, 1);
        lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(col, 0, 0);
        lv_obj_set_style_pad_all(col, 0, 0);
        lv_obj_set_style_pad_row(col, 6, 0);
        lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        ctx->column[line] = col;

        lv_obj_t *title = lv_label_create(col);
        lv_label_set_text_fmt(title, "Line %u", (unsigned)(line + 1));
        lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(title, UI_COLOR_TEXT, 0);

        for (uint8_t c = 0; c < CELL_COUNT; c++) {
            ctx->value[line][c] = make_tile(col, k_cell_caption[c], k_cell_unit[c]);
        }
    }

    return root;
}

static void set_all_dashes(shore_ctx_t *ctx)
{
    for (uint8_t line = 0; line < 2; line++) {
        for (uint8_t c = 0; c < CELL_COUNT; c++) {
            lv_label_set_text(ctx->value[line][c], "--");
            lv_obj_set_style_text_color(ctx->value[line][c], UI_COLOR_TEXT_DIM, 0);
        }
    }
}

void ui_shore_panel_set(lv_obj_t *panel, const ui_shore_reading_t *r, bool valid)
{
    shore_ctx_t *ctx = lv_obj_get_user_data(panel);
    if (ctx == NULL) {
        return;
    }

    if (!valid || r == NULL || r->line_count == 0) {
        /* Both columns stay visible while stale: hiding L2 here would make
         * the layout jump every time the link blips. */
        lv_obj_remove_flag(ctx->column[1], LV_OBJ_FLAG_HIDDEN);
        set_all_dashes(ctx);
        return;
    }

    /* A 30 A pedestal has no Line 2 at all -- hide the column rather than
     * showing zeroes that read as a real measurement. */
    if (r->line_count >= 2) {
        lv_obj_remove_flag(ctx->column[1], LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ctx->column[1], LV_OBJ_FLAG_HIDDEN);
    }

    const lv_color_t color = (r->error_code != 0) ? UI_COLOR_ERR : UI_COLOR_OK;
    char buf[16];

    for (uint8_t line = 0; line < r->line_count && line < 2; line++) {
        snprintf(buf, sizeof(buf), "%.0f", (double)r->volts[line]);
        lv_label_set_text(ctx->value[line][CELL_VOLTS], buf);

        snprintf(buf, sizeof(buf), "%.0f", (double)r->amps[line]);
        lv_label_set_text(ctx->value[line][CELL_AMPS], buf);

        snprintf(buf, sizeof(buf), "%.0f", (double)r->frequency_hz);
        lv_label_set_text(ctx->value[line][CELL_FREQ], buf);

        snprintf(buf, sizeof(buf), "%.0f", (double)r->watts[line]);
        lv_label_set_text(ctx->value[line][CELL_WATTS], buf);

        for (uint8_t c = 0; c < CELL_COUNT; c++) {
            lv_obj_set_style_text_color(ctx->value[line][c], color, 0);
        }
    }
}
