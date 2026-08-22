/*
 * ui — panel screen: status bar, 2x4 button grid (optionally two, see
 * PANEL_HAS_SCREEN_2), idle auto-dim. Runs in the LVGL task on core 1.
 *
 * Touch never talks to TWAI (or ESP-NOW) directly: button callbacks post
 * through bridge_enqueue_dimmer_cmd(), which resolves at build time to
 * either the real CAN TX queue or an ESP-NOW frame to the bridge panel
 * (see PANEL_HAS_CAN, main/bridge_tx.c). Visual state is driven only by
 * ui_on_status(), i.e. by DC_DIMMER_STATUS_3 frames — relayed over
 * ESP-NOW on a remote panel, but never spoofed locally either way.
 * Tank-level widgets are driven separately by ui_on_tank_status(), fed by
 * TANK_STATUS frames — a different DGN/namespace, never routed through
 * ui_on_status().
 *
 * Screen 2 (PANEL_HAS_SCREEN_2) is either a tank readout (mid_coach's
 * SeeLevel FRESH/GREY/BLACK gauges, three genuinely separate tanks side by
 * side) or a battery-bank readout (bedroom_remote's single combined
 * JBD-BMS summary — the three packs are wired in parallel, so they are one
 * bank, not three batteries). Both use the same generic
 * build_screen2_row() layout since ui_dimmer_button_create() already
 * dispatches to the right gauge widget by button type, so ui.c stays
 * panel-agnostic rather than hardcoding which panel gets which screen.
 * Whichever screen-2 flavor is showing, idle_timer_cb() switches back to
 * screen 1 once the backlight goes fully idle-off, so a secondary screen
 * is never left showing after the user walks away.
 */
#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "board_4_3b.h"
#include "bridge_tx.h"
#include "panel_config.h"
#include "state_manager.h"
#include "ui_dimmer_button.h"
#include "ui_theme.h"

#if PANEL_HAS_SCREEN_2
#include "jbd_bms_client.h"
#endif

static const char *TAG = "ui";

#define STATUSBAR_H         36
#define IDLE_DIM_TIMEOUT_MS 120000
#define IDLE_OFF_TIMEOUT_MS 300000
#define IDLE_DIM_PERCENT    50

/* Tank status thresholds (percent). "OK" is anything below WARN. */
#define TANK_WARN_PCT  80
#define TANK_FULL_PCT  89 /* i.e. "over 88%" */
#define TANK_STATUS_TIMER_MS 500

#define BATTERY_STATUS_TIMER_MS 1000

/*
 * Shore power (Hughes Power Watchdog, relayed from the basement BLE proxy).
 * Shown in the status bar rather than on a screen of its own: volts and amps
 * per line is a glanceable pair of numbers, and it means any remote panel
 * gets it for free without needing a screen-2 layout.
 *
 * The proxy re-broadcasts every few seconds, so silence longer than this
 * means the proxy is down, out of range, or lost its BLE link -- show "--"
 * rather than a frozen reading that looks live.
 */
#define SHORE_POWER_STALE_MS  20000
#define SHORE_POWER_TIMER_MS  1000

typedef enum {
    BACKLIGHT_NORMAL,
    BACKLIGHT_DIMMED,
    BACKLIGHT_OFF,
} backlight_state_t;

static lv_obj_t *s_buttons[PANEL_BUTTON_COUNT];
static lv_obj_t *s_dim_overlay;
static backlight_state_t s_backlight_state;
static bool s_ui_ready;

#if !PANEL_HAS_CAN
static lv_obj_t       *s_shore_label;
static ui_shore_power_t s_shore;
static bool             s_shore_seen;
static uint32_t         s_shore_last_ms;
#endif

#if PANEL_HAS_SCREEN_2
static lv_obj_t *s_buttons_2[PANEL_BUTTON_COUNT_2];
static lv_obj_t *s_grid1;
static lv_obj_t *s_grid2;
static lv_obj_t *s_tank_status_label;
static bool      s_grey_found, s_black_found;
static uint8_t   s_grey_instance, s_black_instance;
static bool      s_tank_critical;
static bool      s_tank_blink_on;
static bool      s_screen2_is_battery;

static void switch_screen(void);
#endif

/* ------------------------------------------------------- backlight ------ */

/*
 * The 4.3B backlight enable (CH422G EXIO2) is on/off only, so intermediate
 * brightness is emulated with a translucent black overlay on the LVGL top
 * layer; percent == 0 drives EXIO2 low for a real hardware off, not just a
 * fully-opaque overlay. When the hardware PWM TODO in board_4_3b.h is
 * resolved, route percent to board_backlight_set_percent() and drop the
 * overlay opacity.
 */
static void apply_backlight(uint8_t percent)
{
    board_backlight_set_percent(percent);
    if (percent >= 100) {
        lv_obj_add_flag(s_dim_overlay, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_dim_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_opa(s_dim_overlay,
                                (lv_opa_t)(LV_OPA_COVER * (100 - percent) / 100),
                                0);
    }
}

static void dim_overlay_event_cb(lv_event_t *e)
{
    /* Wake touch: restore brightness and swallow the press so the button
     * underneath never fires. The overlay is only CLICKABLE while dimmed
     * or off; at full brightness touches pass straight through it. */
    if (lv_event_get_code(e) == LV_EVENT_PRESSED && s_backlight_state != BACKLIGHT_NORMAL) {
        s_backlight_state = BACKLIGHT_NORMAL;
        lv_obj_remove_flag(s_dim_overlay, LV_OBJ_FLAG_CLICKABLE);
        apply_backlight(100);
    }
}

static void idle_timer_cb(lv_timer_t *t)
{
    (void)t;

#if PANEL_HAS_SCREEN_2
    /* A critical (>88%) grey/black tank overrides idle dimming entirely:
     * this is a "go empty the tank" alert, the screen must stay readable.
     * Only touch the backlight on the transition (or if some other state
     * left it non-normal) to avoid a CH422G write every tick. */
    if (s_tank_critical) {
        if (s_backlight_state != BACKLIGHT_NORMAL) {
            s_backlight_state = BACKLIGHT_NORMAL;
            lv_obj_remove_flag(s_dim_overlay, LV_OBJ_FLAG_CLICKABLE);
            apply_backlight(100);
        }
        return;
    }
#endif

    uint32_t inactive_ms = lv_display_get_inactive_time(NULL);

    if (s_backlight_state == BACKLIGHT_NORMAL && inactive_ms > IDLE_DIM_TIMEOUT_MS) {
        s_backlight_state = BACKLIGHT_DIMMED;
        apply_backlight(IDLE_DIM_PERCENT);
        lv_obj_add_flag(s_dim_overlay, LV_OBJ_FLAG_CLICKABLE);
        ESP_LOGI(TAG, "idle %lu ms -> dimmed", (unsigned long)inactive_ms);
    } else if (s_backlight_state == BACKLIGHT_DIMMED && inactive_ms > IDLE_OFF_TIMEOUT_MS) {
        s_backlight_state = BACKLIGHT_OFF;
        apply_backlight(0);
        ESP_LOGI(TAG, "idle %lu ms -> backlight off", (unsigned long)inactive_ms);
#if PANEL_HAS_SCREEN_2
        /* Screen actually going dark -- don't leave a secondary screen
         * (tank/battery readout) showing for whoever glances at it next;
         * fall back to the primary light-button grid. */
        if (s_grid2 != NULL && !lv_obj_has_flag(s_grid2, LV_OBJ_FLAG_HIDDEN)) {
            switch_screen();
        }
#endif
    }
}

#if PANEL_HAS_SCREEN_2
/* ------------------------------------------------------- tank status ---- */

static void tank_status_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_tank_status_label == NULL || !s_grey_found || !s_black_found) {
        return;
    }

    uint8_t grey_pct = 0, black_pct = 0;
    bool grey_valid = state_manager_get_tank(s_grey_instance, &grey_pct);
    bool black_valid = state_manager_get_tank(s_black_instance, &black_pct);

    s_tank_blink_on = !s_tank_blink_on;

    if (!grey_valid || !black_valid) {
        s_tank_critical = false;
        lv_label_set_text(s_tank_status_label, "Grey-Black --");
        lv_obj_set_style_text_color(s_tank_status_label, UI_COLOR_TEXT_DIM, 0);
        lv_obj_remove_flag(s_tank_status_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    const uint8_t worst = grey_pct > black_pct ? grey_pct : black_pct;

    if (worst >= TANK_FULL_PCT) {
        s_tank_critical = true;
        lv_label_set_text(s_tank_status_label, "Grey-Black FULL");
        lv_obj_set_style_text_color(s_tank_status_label, UI_COLOR_ERR, 0);
        /* Blink by toggling visibility every tick. */
        if (s_tank_blink_on) {
            lv_obj_remove_flag(s_tank_status_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_tank_status_label, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (worst >= TANK_WARN_PCT) {
        s_tank_critical = false;
        lv_label_set_text(s_tank_status_label, "Grey-Black Warn");
        lv_obj_set_style_text_color(s_tank_status_label, UI_COLOR_WARN, 0);
        lv_obj_remove_flag(s_tank_status_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        s_tank_critical = false;
        lv_label_set_text(s_tank_status_label, "Grey-Black OK");
        lv_obj_set_style_text_color(s_tank_status_label, UI_COLOR_TEXT, 0);
        lv_obj_remove_flag(s_tank_status_label, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ----------------------------------------------------- battery status --- */

/* Kconfig MAC per battery slot. The all-zero placeholder means "slot not
 * configured" -- it is never connected to and never counted toward the
 * bank's "N of M" indicator. */
static const char *const k_battery_macs[JBD_BMS_MAX_BATTERIES] = {
    CONFIG_FIREFLY_BATTERY_1_MAC,
    CONFIG_FIREFLY_BATTERY_2_MAC,
    CONFIG_FIREFLY_BATTERY_3_MAC,
};

static bool battery_slot_configured(uint8_t index)
{
    return strcmp(k_battery_macs[index], "00:00:00:00:00:00") != 0;
}

/*
 * Polls every configured pack, combines the live ones into a single bank
 * reading, and pushes that plus the per-pack detail to the summary widget.
 *
 * The packs are wired in parallel, so this is deliberately one reading, not
 * three: jbd_bms_combine() does the aggregation (pure C, host-tested), and
 * only packs that answered are handed to it, so a pack dropping off BLE
 * shrinks the bank rather than dragging the averages toward zero.
 */
static void battery_status_timer_cb(lv_timer_t *t)
{
    (void)t;

    jbd_bms_status_t       live[JBD_BMS_MAX_BATTERIES];
    ui_battery_pack_info_t detail[JBD_BMS_MAX_BATTERIES];
    uint8_t live_count = 0;
    uint8_t configured_count = 0;

    memset(detail, 0, sizeof(detail));

    for (uint8_t i = 0; i < JBD_BMS_MAX_BATTERIES; i++) {
        detail[i].configured = battery_slot_configured(i);
        snprintf(detail[i].mac, sizeof(detail[i].mac), "%s", k_battery_macs[i]);
        if (!detail[i].configured) {
            continue;
        }
        configured_count++;

        jbd_bms_status_t status;
        if (jbd_bms_get_status(i, &status) && jbd_bms_healthy(i)) {
            detail[i].online = true;
            detail[i].status = status;
            live[live_count++] = status;
        }
    }

    jbd_bms_bank_t bank;
    const bool have_bank = jbd_bms_combine(live, live_count, &bank);

    for (uint32_t j = 0; j < PANEL_BUTTON_COUNT_2; j++) {
        if (s_buttons_2[j] != NULL) {
            ui_dimmer_button_update_bank(s_buttons_2[j],
                                         have_bank ? &bank : NULL,
                                         detail, JBD_BMS_MAX_BATTERIES,
                                         configured_count);
        }
    }
}
#endif

#if !PANEL_HAS_CAN
/* ------------------------------------------------------- shore power ---- */

static void shore_power_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_shore_label == NULL) {
        return;
    }

    const bool stale = !s_shore_seen ||
                       (lv_tick_get() - s_shore_last_ms) > SHORE_POWER_STALE_MS;
    if (stale) {
        lv_label_set_text(s_shore_label, "Shore --");
        lv_obj_set_style_text_color(s_shore_label, UI_COLOR_TEXT_DIM, 0);
        return;
    }

    char buf[48];
    if (s_shore.line_count >= 2) {
        snprintf(buf, sizeof(buf), "%.0fV %.0fA  %.0fV %.0fA",
                 (double)s_shore.volts[0], (double)s_shore.amps[0],
                 (double)s_shore.volts[1], (double)s_shore.amps[1]);
    } else {
        snprintf(buf, sizeof(buf), "%.0fV %.0fA",
                 (double)s_shore.volts[0], (double)s_shore.amps[0]);
    }
    lv_label_set_text(s_shore_label, buf);
    /* The Watchdog's own error code is the authoritative "something is
     * wrong with the pedestal" signal -- surface it as color rather than
     * trying to re-derive limits from the raw volts here. */
    lv_obj_set_style_text_color(s_shore_label,
                                s_shore.error_code != 0 ? UI_COLOR_ERR : UI_COLOR_TEXT, 0);
}
#endif

/* --------------------------------------------------------- screen nav --- */

#if PANEL_HAS_SCREEN_2
static void switch_screen(void)
{
    if (lv_obj_has_flag(s_grid1, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_remove_flag(s_grid1, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_grid2, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_grid1, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_grid2, LV_OBJ_FLAG_HIDDEN);
    }
}
#endif

/* ------------------------------------------------------- commands ------- */

static void panel_send_cb(const panel_btn_def_t *def, rvc_dimmer_cmd_t cmd,
                          void *user_ctx)
{
    (void)user_ctx;

    if (def->type == PANEL_BTN_SCREEN_SWITCH) {
#if PANEL_HAS_SCREEN_2
        switch_screen();
#endif
        return;
    }
    if (def->type == PANEL_BTN_TANK_LEVEL || def->type == PANEL_BTN_SPACER) {
        return;   /* read-only / no widget — never reaches here in practice */
    }

    /* ON/OFF/TOGGLE carry an explicit desired level of 100 % rather than
     * 0xFF "no change" — this matches the proven-working frame from
     * rvc-proxy/CoachProxy ([inst FF C8 cmd FF 00 FF FF]) observed to
     * actuate loads instantly on real coaches. Ramp/stop commands keep
     * level = NA; the ramp itself defines the level trajectory. */
    uint8_t level = RVC_FIELD_NA;
    if (cmd == RVC_DIMMER_CMD_ON_DELAY || cmd == RVC_DIMMER_CMD_OFF ||
        cmd == RVC_DIMMER_CMD_TOGGLE) {
        level = RVC_LEVEL_MAX;
    }

    for (uint8_t i = 0; i < def->instance_count; i++) {
        bridge_enqueue_dimmer_cmd(def->instances[i], cmd,
                                  level, RVC_FIELD_NA);
    }
}

/* ------------------------------------------------------- screen build --- */

/*
 * Populates one 2x4 button grid into `parent` (already sized/positioned by
 * the caller) from `buttons`/`count`, filling `out_buttons` in the same
 * order. A PANEL_BTN_SPACER entry gets no widget (out_buttons[i] = NULL,
 * cell stays visually empty) but still consumes its grid slot, so it's how
 * a panel positions a button (e.g. a screen-switch "back" button) at a
 * specific cell like the bottom-right one instead of wherever sequential
 * fill would put it.
 */
static void build_button_grid(lv_obj_t *parent, const panel_btn_def_t *buttons,
                              uint32_t count, lv_obj_t **out_buttons)
{
    static int32_t col_dsc[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
    static int32_t row_dsc[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
                                 LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };

    lv_obj_set_grid_dsc_array(parent, col_dsc, row_dsc);
    lv_obj_set_style_pad_all(parent, 3, 0);
    lv_obj_set_style_pad_column(parent, 3, 0);
    lv_obj_set_style_pad_row(parent, 3, 0);

    for (uint32_t i = 0; i < count; i++) {
        if (buttons[i].type == PANEL_BTN_SPACER) {
            out_buttons[i] = NULL;
            continue;
        }
        lv_obj_t *btn = ui_dimmer_button_create(parent, &buttons[i],
                                                panel_send_cb, NULL);
        lv_obj_set_grid_cell(btn,
                             LV_GRID_ALIGN_STRETCH, i % 2, 1,
                             LV_GRID_ALIGN_STRETCH, i / 2, 1);
        out_buttons[i] = btn;
    }
}

#if PANEL_HAS_SCREEN_2
/*
 * Screen 2 is a row of read-only gauges -- tank levels (mid_coach) or
 * battery status (bedroom_remote), see the file header note. Both
 * PANEL_BTN_TANK_LEVEL and PANEL_BTN_BATTERY_STATUS entries are laid out
 * identically as a centered horizontal row (ui_dimmer_button_create()
 * already dispatches to the right gauge widget by button type), and the
 * one PANEL_BTN_SCREEN_SWITCH entry ("BACK") is a small button pinned to
 * the bottom center, rather than an equal-sized grid cell like screen 1's
 * buttons. PANEL_BTN_SPACER entries, if any, are skipped -- the row layout
 * doesn't need manual gap positioning.
 */
static void build_screen2_row(lv_obj_t *parent, const panel_btn_def_t *buttons,
                              uint32_t count, lv_obj_t **out_buttons)
{
    /* The tank gauges are tall thin columns that look right in a 75%-height
     * row, but the battery bank readout is one full-width card with four
     * stacked sections -- at 75% it leaves an obvious dead band above the
     * BACK button. Give it the extra height instead of padding the widget
     * out internally. */
    bool has_summary = false;
    for (uint32_t i = 0; i < count; i++) {
        if (buttons[i].type == PANEL_BTN_BATTERY_SUMMARY) {
            has_summary = true;
            break;
        }
    }

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_add_style(row, &ui_style_screen, 0);
    lv_obj_set_size(row, LV_PCT(100), has_summary ? LV_PCT(89) : LV_PCT(75));
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    for (uint32_t i = 0; i < count; i++) {
        if (buttons[i].type == PANEL_BTN_SPACER) {
            out_buttons[i] = NULL;
            continue;
        }
        if (buttons[i].type == PANEL_BTN_SCREEN_SWITCH) {
            lv_obj_t *btn = ui_dimmer_button_create(parent, &buttons[i],
                                                    panel_send_cb, NULL);
            lv_obj_set_size(btn, 130, 44);
            lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -8);
            out_buttons[i] = btn;
            continue;
        }
        lv_obj_t *btn = ui_dimmer_button_create(row, &buttons[i],
                                                panel_send_cb, NULL);
        if (buttons[i].type == PANEL_BTN_BATTERY_SUMMARY) {
            /* One combined readout for the whole bank, so it owns the full
             * area rather than sharing the row -- unlike the tank gauges,
             * which really are three separate things side by side. */
            lv_obj_set_size(btn, LV_PCT(100), LV_PCT(100));
        } else {
            lv_obj_set_size(btn, 140, LV_PCT(100));
        }
        out_buttons[i] = btn;
    }
}
#endif

static void build_screen(void)
{
    ui_theme_init();

    lv_obj_t *scr = lv_screen_active();
    lv_obj_add_style(scr, &ui_style_screen, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* --- status bar --- */
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_add_style(bar, &ui_style_statusbar, 0);
    lv_obj_set_size(bar, LV_PCT(100), STATUSBAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(bar);
    lv_label_set_text(title, PANEL_NAME);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);

#if !PANEL_HAS_CAN
    /* Remote panels have no tank-status readout competing for the right
     * side of the status bar (that only exists on a panel with GREY/BLACK
     * tank buttons, i.e. a CAN panel), so shore power lives there. */
    s_shore_label = lv_label_create(bar);
    lv_label_set_text(s_shore_label, "Shore --");
    lv_obj_set_style_text_color(s_shore_label, UI_COLOR_TEXT_DIM, 0);
    lv_obj_align(s_shore_label, LV_ALIGN_RIGHT_MID, 0, 0);
#endif

#if PANEL_HAS_SCREEN_2
    /* Find the GREY/BLACK tank buttons by label rather than hardcoding
     * instance numbers here -- keeps ui.c panel-agnostic, consistent with
     * the rest of this file. If a panel with screen 2 has no such buttons
     * (not the case today), the header simply shows nothing. */
    for (uint32_t i = 0; i < PANEL_BUTTON_COUNT_2; i++) {
        const panel_btn_def_t *def = &PANEL_BUTTONS_2[i];
        if (def->type != PANEL_BTN_TANK_LEVEL || def->instance_count == 0) {
            continue;
        }
        if (strcmp(def->label, "GREY") == 0) {
            s_grey_instance = def->instances[0];
            s_grey_found = true;
        } else if (strcmp(def->label, "BLACK") == 0) {
            s_black_instance = def->instances[0];
            s_black_found = true;
        }
    }
    if (s_grey_found && s_black_found) {
        s_tank_status_label = lv_label_create(bar);
        lv_label_set_text(s_tank_status_label, "Grey-Black --");
        lv_obj_set_style_text_color(s_tank_status_label, UI_COLOR_TEXT_DIM, 0);
        lv_obj_align(s_tank_status_label, LV_ALIGN_RIGHT_MID, 0, 0);
    }

    for (uint32_t i = 0; i < PANEL_BUTTON_COUNT_2; i++) {
        if (PANEL_BUTTONS_2[i].type == PANEL_BTN_BATTERY_SUMMARY) {
            s_screen2_is_battery = true;
            break;
        }
    }
#endif

    /* Use logical height (post-rotation) so the grid fills correctly in
     * both landscape (480 px) and portrait (800 px) orientations. */
    int32_t logical_h = (int32_t)lv_display_get_vertical_resolution(NULL);

    /* --- screen 1 (always present) --- */
    lv_obj_t *grid1 = lv_obj_create(scr);
    lv_obj_add_style(grid1, &ui_style_screen, 0);
    lv_obj_set_size(grid1, LV_PCT(100), logical_h - STATUSBAR_H);
    lv_obj_align(grid1, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_remove_flag(grid1, LV_OBJ_FLAG_SCROLLABLE);
    build_button_grid(grid1, PANEL_BUTTONS, PANEL_BUTTON_COUNT, s_buttons);

#if PANEL_HAS_SCREEN_2
    /* --- screen 2 (optional second grid, hidden until switch_screen()) ---
     * Built now, not lazily on first switch, so status updates keep both
     * screens' widgets correct even while one is hidden — a button must
     * never show stale state just because you weren't looking at it. */
    s_grid1 = grid1;
    lv_obj_t *grid2 = lv_obj_create(scr);
    lv_obj_add_style(grid2, &ui_style_screen, 0);
    lv_obj_set_size(grid2, LV_PCT(100), logical_h - STATUSBAR_H);
    lv_obj_align(grid2, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_remove_flag(grid2, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(grid2, LV_OBJ_FLAG_HIDDEN);
    build_screen2_row(grid2, PANEL_BUTTONS_2, PANEL_BUTTON_COUNT_2, s_buttons_2);
    s_grid2 = grid2;
#endif

    /* --- idle-dim overlay (top layer, above everything) --- */
    s_dim_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_dim_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_dim_overlay, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_dim_overlay, 0, 0);
    lv_obj_set_style_radius(s_dim_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_dim_overlay, 0, 0);
    lv_obj_remove_flag(s_dim_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_dim_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_dim_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_dim_overlay, dim_overlay_event_cb, LV_EVENT_PRESSED, NULL);

    lv_timer_create(idle_timer_cb, 1000, NULL);
#if !PANEL_HAS_CAN
    lv_timer_create(shore_power_timer_cb, SHORE_POWER_TIMER_MS, NULL);
#endif
#if PANEL_HAS_SCREEN_2
    if (s_tank_status_label != NULL) {
        lv_timer_create(tank_status_timer_cb, TANK_STATUS_TIMER_MS, NULL);
    }
    if (s_screen2_is_battery) {
        lv_timer_create(battery_status_timer_cb, BATTERY_STATUS_TIMER_MS, NULL);
    }
#endif
}

/* ------------------------------------------------------------- API ------ */

void ui_init(void)
{
    lvgl_port_lock(0);
    build_screen();
    lvgl_port_unlock();
    s_ui_ready = true;
#if PANEL_HAS_SCREEN_2
    ESP_LOGI(TAG, "UI ready: %s (%u buttons, +%u on screen 2)", PANEL_NAME,
             (unsigned)PANEL_BUTTON_COUNT, (unsigned)PANEL_BUTTON_COUNT_2);
#else
    ESP_LOGI(TAG, "UI ready: %s (%u buttons)", PANEL_NAME,
             (unsigned)PANEL_BUTTON_COUNT);
#endif
}

void ui_on_status(uint8_t instance, uint8_t level, bool on)
{
    if (!s_ui_ready) {
        return;
    }
    lvgl_port_lock(0);
    for (uint32_t i = 0; i < PANEL_BUTTON_COUNT; i++) {
        if (s_buttons[i] != NULL) {
            ui_dimmer_button_update(s_buttons[i], instance, level, on);
        }
    }
#if PANEL_HAS_SCREEN_2
    for (uint32_t i = 0; i < PANEL_BUTTON_COUNT_2; i++) {
        if (s_buttons_2[i] != NULL) {
            ui_dimmer_button_update(s_buttons_2[i], instance, level, on);
        }
    }
#endif
    lvgl_port_unlock();
}

void ui_on_shore_power(const ui_shore_power_t *sp)
{
#if !PANEL_HAS_CAN
    if (!s_ui_ready || sp == NULL) {
        return;
    }
    /* Only the cached values and timestamp are touched here; the label is
     * repainted by shore_power_timer_cb, which also owns the staleness
     * decision. One place decides what the readout says. */
    lvgl_port_lock(0);
    s_shore = *sp;
    s_shore_seen = true;
    s_shore_last_ms = lv_tick_get();
    lvgl_port_unlock();
#else
    (void)sp;
#endif
}

void ui_on_tank_status(uint8_t instance, uint8_t percent, bool valid)
{
    if (!s_ui_ready) {
        return;
    }
    lvgl_port_lock(0);
#if PANEL_HAS_SCREEN_2
    for (uint32_t i = 0; i < PANEL_BUTTON_COUNT_2; i++) {
        if (s_buttons_2[i] != NULL) {
            ui_dimmer_button_update_tank(s_buttons_2[i], instance, percent, valid);
        }
    }
#endif
    for (uint32_t i = 0; i < PANEL_BUTTON_COUNT; i++) {
        if (s_buttons[i] != NULL) {
            ui_dimmer_button_update_tank(s_buttons[i], instance, percent, valid);
        }
    }
    lvgl_port_unlock();
}
