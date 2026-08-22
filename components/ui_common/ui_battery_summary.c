#include "ui_battery_summary.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui_theme.h"

/* SOC color bands, shared by the arc indicator and the percent label. */
#define SOC_BAND_OK_PCT   50
#define SOC_BAND_WARN_PCT 20

/* Matches jbd_bms_estimate_hours()'s own idle threshold, so the direction
 * caption and the ETA never disagree about whether the bank is at rest. */
#define IDLE_THRESHOLD_A 0.05f

/* Beyond this an ETA is noise (a near-idle bank divides out to hundreds of
 * hours); show a ceiling marker instead of a fake-precise number. */
#define ETA_MAX_HOURS 99.0f

#define DETAIL_MAX_PACKS 3

typedef struct {
    lv_obj_t *arc;
    lv_obj_t *pct_label;

    lv_obj_t *volts_value;
    lv_obj_t *power_value;
    lv_obj_t *current_value;
    lv_obj_t *current_word;   /* Charging / Discharging / Idle */
    lv_obj_t *eta_caption;    /* flips Fully Charged In / Time Remaining / Idle */
    lv_obj_t *eta_value;

    lv_obj_t *temp_label;
    lv_obj_t *packs_label;

    lv_obj_t *popup;
    lv_obj_t *popup_rows[DETAIL_MAX_PACKS];

    ui_battery_pack_info_t packs[DETAIL_MAX_PACKS];
    uint8_t                pack_count;
} summary_ctx_t;

/* ------------------------------------------------------------ helpers --- */

static lv_color_t soc_color(uint8_t percent)
{
    if (percent >= SOC_BAND_OK_PCT) {
        return UI_COLOR_OK;
    }
    if (percent >= SOC_BAND_WARN_PCT) {
        return UI_COLOR_WARN;
    }
    return UI_COLOR_ERR;
}

/* "13h 20m" / "45m" / ">99h" / "--", the Vatrer-style duration rendering --
 * a bare "13.3h" reads as a measurement rather than a time. */
static void format_hours(char *buf, size_t len, float hours)
{
    if (!isfinite(hours) || hours < 0.0f) {
        snprintf(buf, len, "--");
        return;
    }
    if (hours > ETA_MAX_HOURS) {
        snprintf(buf, len, ">%dh", (int)ETA_MAX_HOURS);
        return;
    }
    const int total_minutes = (int)(hours * 60.0f + 0.5f);
    const int h = total_minutes / 60;
    const int m = total_minutes % 60;
    if (h == 0) {
        snprintf(buf, len, "%dm", m);
    } else {
        snprintf(buf, len, "%dh %02dm", h, m);
    }
}

/* One caption-over-value cell of the 2x2 readout grid. Returns the cell so
 * a caller can append an extra line under the value (see the direction word
 * under Total Current). */
static lv_obj_t *make_cell(lv_obj_t *parent, uint8_t col, uint8_t row,
                           const char *caption, lv_obj_t **out_caption,
                           lv_obj_t **out_value)
{
    lv_obj_t *cell = lv_obj_create(parent);
    lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cell, 0, 0);
    lv_obj_set_style_pad_all(cell, 2, 0);
    lv_obj_set_style_pad_row(cell, 2, 0);
    lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(cell, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_grid_cell(cell, LV_GRID_ALIGN_STRETCH, col, 1,
                         LV_GRID_ALIGN_STRETCH, row, 1);

    lv_obj_t *cap = lv_label_create(cell);
    lv_label_set_text(cap, caption);
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cap, UI_COLOR_TEXT_DIM, 0);

    lv_obj_t *val = lv_label_create(cell);
    lv_label_set_text(val, "--");
    lv_obj_set_style_text_font(val, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(val, UI_COLOR_TEXT, 0);

    if (out_caption != NULL) {
        *out_caption = cap;
    }
    *out_value = val;
    return cell;
}

/* ------------------------------------------------------------- popup ---- */

static void popup_click_cb(lv_event_t *e)
{
    lv_obj_add_flag(lv_event_get_target(e), LV_OBJ_FLAG_HIDDEN);
}

static void refresh_popup_rows(summary_ctx_t *ctx)
{
    if (ctx->popup == NULL) {
        return;
    }
    for (uint8_t i = 0; i < DETAIL_MAX_PACKS; i++) {
        lv_obj_t *row = ctx->popup_rows[i];
        if (row == NULL) {
            continue;
        }
        if (i >= ctx->pack_count || !ctx->packs[i].configured) {
            lv_label_set_text_fmt(row, "%u  not configured", (unsigned)(i + 1));
            lv_obj_set_style_text_color(row, UI_COLOR_TEXT_DIM, 0);
            continue;
        }

        const ui_battery_pack_info_t *p = &ctx->packs[i];
        if (!p->online) {
            lv_label_set_text_fmt(row, "%u  %s\n   offline",
                                  (unsigned)(i + 1), p->mac);
            lv_obj_set_style_text_color(row, UI_COLOR_ERR, 0);
            continue;
        }

        char temp[16];
        if (p->status.temp_count > 0) {
            snprintf(temp, sizeof(temp), "  %.0fF",
                     (double)jbd_bms_c_to_f(p->status.temp_c[0]));
        } else {
            temp[0] = '\0';
        }
        lv_label_set_text_fmt(row, "%u  %s\n   %u%%  %.2fV  %.1fA%s",
                              (unsigned)(i + 1), p->mac,
                              (unsigned)p->status.soc_percent,
                              (double)p->status.voltage_v,
                              (double)p->status.current_a,
                              temp);
        lv_obj_set_style_text_color(row, UI_COLOR_TEXT, 0);
    }
}

static void build_popup(summary_ctx_t *ctx)
{
    lv_obj_t *popup = lv_obj_create(lv_layer_top());
    lv_obj_set_style_bg_color(popup, UI_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(popup, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(popup, 2, 0);
    lv_obj_set_style_border_color(popup, UI_COLOR_OFF, 0);
    lv_obj_set_style_radius(popup, 10, 0);
    lv_obj_set_style_pad_all(popup, 14, 0);
    lv_obj_set_style_pad_row(popup, 10, 0);
    lv_obj_set_size(popup, LV_PCT(92), LV_SIZE_CONTENT);
    lv_obj_remove_flag(popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(popup, LV_FLEX_FLOW_COLUMN);
    lv_obj_center(popup);
    lv_obj_add_event_cb(popup, popup_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = lv_label_create(popup);
    lv_label_set_text(title, "PACK DETAIL");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, UI_COLOR_TEXT_DIM, 0);

    for (uint8_t i = 0; i < DETAIL_MAX_PACKS; i++) {
        lv_obj_t *row = lv_label_create(popup);
        lv_label_set_text(row, "--");
        lv_obj_set_style_text_font(row, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(row, UI_COLOR_TEXT, 0);
        ctx->popup_rows[i] = row;
    }

    lv_obj_t *hint = lv_label_create(popup);
    lv_label_set_text(hint, "tap to close");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, UI_COLOR_TEXT_DIM, 0);

    ctx->popup = popup;
    refresh_popup_rows(ctx);
}

/* ------------------------------------------------------------ lifecycle - */

static void summary_delete_cb(lv_event_t *e)
{
    summary_ctx_t *ctx = lv_event_get_user_data(e);
    if (ctx->popup != NULL) {
        lv_obj_delete(ctx->popup);
    }
    lv_free(ctx);
}

lv_obj_t *ui_battery_summary_create(lv_obj_t *parent)
{
    summary_ctx_t *ctx = lv_malloc(sizeof(summary_ctx_t));
    LV_ASSERT_MALLOC(ctx);
    memset(ctx, 0, sizeof(*ctx));

    /* Transparent wrapper -- the parent button already carries the card
     * background, same arrangement the per-pack gauge used. */
    lv_obj_t *wrapper = lv_obj_create(parent);
    lv_obj_set_style_bg_opa(wrapper, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wrapper, 0, 0);
    lv_obj_set_size(wrapper, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(wrapper, 4, 0);
    lv_obj_set_style_pad_row(wrapper, 6, 0);
    lv_obj_remove_flag(wrapper, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(wrapper, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(wrapper, LV_FLEX_FLOW_COLUMN);
    /* Every section is a fixed height and the slack goes between them, so
     * the three bands stay visually distinct. (Letting the grid flex-grow
     * instead just stretches its two rows apart until the pairs stop
     * reading as a group.) */
    lv_obj_set_flex_align(wrapper, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_user_data(wrapper, ctx);
    lv_obj_add_event_cb(wrapper, summary_delete_cb, LV_EVENT_DELETE, ctx);

    /* --- SOC arc, the visual anchor (mirrors the Vatrer ring) --- */
    ctx->arc = lv_arc_create(wrapper);
    lv_obj_set_size(ctx->arc, 210, 210);
    lv_arc_set_range(ctx->arc, 0, 100);
    lv_arc_set_value(ctx->arc, 0);
    lv_arc_set_rotation(ctx->arc, 135);
    lv_arc_set_bg_angles(ctx->arc, 0, 270);
    lv_obj_remove_style(ctx->arc, NULL, LV_PART_KNOB);
    /* Read-only: the tap must reach the parent button (detail popup), not
     * be swallowed as an arc drag. */
    lv_obj_remove_flag(ctx->arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(ctx->arc, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ctx->arc, 14, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ctx->arc, UI_COLOR_TANK_EMPTY, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ctx->arc, UI_COLOR_OK, LV_PART_INDICATOR);

    ctx->pct_label = lv_label_create(ctx->arc);
    lv_label_set_text(ctx->pct_label, "--");
    lv_obj_set_style_text_font(ctx->pct_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(ctx->pct_label, UI_COLOR_TEXT, 0);
    lv_obj_center(ctx->pct_label);

    /* --- 2x2 readout grid --- */
    static int32_t col_dsc[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
    static int32_t row_dsc[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };

    lv_obj_t *grid = lv_obj_create(wrapper);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_size(grid, LV_PCT(100), 200);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_grid_dsc_array(grid, col_dsc, row_dsc);

    make_cell(grid, 0, 0, "Total Voltage", NULL, &ctx->volts_value);
    make_cell(grid, 1, 0, "Total Power",   NULL, &ctx->power_value);
    lv_obj_t *current_cell =
        make_cell(grid, 0, 1, "Total Current", NULL, &ctx->current_value);
    make_cell(grid, 1, 1, "Time Remaining", &ctx->eta_caption, &ctx->eta_value);

    /* Direction word under the current reading. The current value itself is
     * shown as a magnitude, so this is what carries the sign. */
    ctx->current_word = lv_label_create(current_cell);
    lv_label_set_text(ctx->current_word, "");
    lv_obj_set_style_text_font(ctx->current_word, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(ctx->current_word, UI_COLOR_TEXT_DIM, 0);

    /* --- bottom strip: temperature + pack count --- */
    lv_obj_t *strip = lv_obj_create(wrapper);
    lv_obj_set_style_bg_opa(strip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(strip, 0, 0);
    lv_obj_set_style_pad_all(strip, 0, 0);
    lv_obj_set_size(strip, LV_PCT(100), 30);
    lv_obj_remove_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(strip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(strip, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    ctx->temp_label = lv_label_create(strip);
    lv_label_set_text(ctx->temp_label, "--");
    lv_obj_set_style_text_font(ctx->temp_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(ctx->temp_label, UI_COLOR_TEXT_DIM, 0);

    ctx->packs_label = lv_label_create(strip);
    lv_label_set_text(ctx->packs_label, "--");
    lv_obj_set_style_text_font(ctx->packs_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(ctx->packs_label, UI_COLOR_TEXT_DIM, 0);

    return wrapper;
}

/* ---------------------------------------------------------------- API --- */

void ui_battery_summary_set_bank(lv_obj_t *summary, const jbd_bms_bank_t *bank,
                                 uint8_t configured_packs)
{
    summary_ctx_t *ctx = lv_obj_get_user_data(summary);
    if (ctx == NULL) {
        return;
    }

    char buf[24];

    if (bank == NULL || bank->pack_count == 0) {
        lv_label_set_text(ctx->pct_label, "--");
        lv_arc_set_value(ctx->arc, 0);
        lv_obj_set_style_arc_color(ctx->arc, UI_COLOR_OFF, LV_PART_INDICATOR);
        lv_label_set_text(ctx->volts_value, "--");
        lv_label_set_text(ctx->power_value, "--");
        lv_label_set_text(ctx->current_value, "--");
        lv_label_set_text(ctx->current_word, "");
        lv_label_set_text(ctx->eta_caption, "Time Remaining");
        lv_label_set_text(ctx->eta_value, "--");
        lv_label_set_text(ctx->temp_label, "--");
        snprintf(buf, sizeof(buf), "0 of %u", (unsigned)configured_packs);
        lv_label_set_text(ctx->packs_label, buf);
        lv_obj_set_style_text_color(ctx->packs_label,
                                    configured_packs > 0 ? UI_COLOR_ERR
                                                         : UI_COLOR_TEXT_DIM, 0);
        return;
    }

    const lv_color_t band = soc_color(bank->soc_percent);
    lv_arc_set_value(ctx->arc, bank->soc_percent);
    lv_obj_set_style_arc_color(ctx->arc, band, LV_PART_INDICATOR);
    snprintf(buf, sizeof(buf), "%u%%", (unsigned)bank->soc_percent);
    lv_label_set_text(ctx->pct_label, buf);

    snprintf(buf, sizeof(buf), "%.2fV", (double)bank->voltage_v);
    lv_label_set_text(ctx->volts_value, buf);

    /* Power and current are shown as magnitudes -- the direction is carried
     * by the ETA caption below, so a leading minus sign would be redundant
     * and easy to misread at a glance. */
    snprintf(buf, sizeof(buf), "%.0fW", (double)fabsf(bank->power_w));
    lv_label_set_text(ctx->power_value, buf);

    snprintf(buf, sizeof(buf), "%.1fA", (double)fabsf(bank->current_a));
    lv_label_set_text(ctx->current_value, buf);

    /* One direction decision drives both the word under Total Current and
     * the ETA caption, so the two can never contradict each other. */
    if (bank->current_a > IDLE_THRESHOLD_A) {
        lv_label_set_text(ctx->current_word, "Charging");
        lv_obj_set_style_text_color(ctx->current_word, UI_COLOR_OK, 0);
        lv_label_set_text(ctx->eta_caption, "Fully Charged In");
    } else if (bank->current_a < -IDLE_THRESHOLD_A) {
        lv_label_set_text(ctx->current_word, "Discharging");
        lv_obj_set_style_text_color(ctx->current_word, UI_COLOR_AMBER, 0);
        lv_label_set_text(ctx->eta_caption, "Time Remaining");
    } else {
        lv_label_set_text(ctx->current_word, "Idle");
        lv_obj_set_style_text_color(ctx->current_word, UI_COLOR_TEXT_DIM, 0);
        lv_label_set_text(ctx->eta_caption, "Idle");
    }
    format_hours(buf, sizeof(buf), bank->hours);
    lv_label_set_text(ctx->eta_value, buf);

    if (bank->temp_valid) {
        snprintf(buf, sizeof(buf), "%.0fF / %.0fF",
                 (double)jbd_bms_c_to_f(bank->temp_max_c),
                 (double)jbd_bms_c_to_f(bank->temp_min_c));
        lv_label_set_text(ctx->temp_label, buf);
    } else {
        lv_label_set_text(ctx->temp_label, "--");
    }

    snprintf(buf, sizeof(buf), "%u of %u", (unsigned)bank->pack_count,
             (unsigned)configured_packs);
    lv_label_set_text(ctx->packs_label, buf);
    lv_obj_set_style_text_color(ctx->packs_label,
                                bank->pack_count < configured_packs
                                    ? UI_COLOR_WARN : UI_COLOR_TEXT_DIM, 0);
}

void ui_battery_summary_set_packs(lv_obj_t *summary,
                                  const ui_battery_pack_info_t *packs,
                                  uint8_t count)
{
    summary_ctx_t *ctx = lv_obj_get_user_data(summary);
    if (ctx == NULL || packs == NULL) {
        return;
    }
    if (count > DETAIL_MAX_PACKS) {
        count = DETAIL_MAX_PACKS;
    }
    memcpy(ctx->packs, packs, (size_t)count * sizeof(ctx->packs[0]));
    ctx->pack_count = count;
    refresh_popup_rows(ctx);
}

void ui_battery_summary_toggle_detail(lv_obj_t *summary)
{
    summary_ctx_t *ctx = lv_obj_get_user_data(summary);
    if (ctx == NULL) {
        return;
    }
    if (ctx->popup == NULL) {
        build_popup(ctx);
        return;   /* created visible */
    }
    if (lv_obj_has_flag(ctx->popup, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_remove_flag(ctx->popup, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ctx->popup, LV_OBJ_FLAG_HIDDEN);
    }
}
