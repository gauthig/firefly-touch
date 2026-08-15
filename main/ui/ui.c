/*
 * ui — panel screen: status bar, 2x4 button grid (optionally two, see
 * PANEL_HAS_SCREEN_2), link-health dot, idle auto-dim. Runs in the LVGL
 * task on core 1.
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
 */
#include "ui.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "board_4_3b.h"
#include "bridge_tx.h"
#include "panel_config.h"
#include "state_manager.h"
#include "ui_dimmer_button.h"
#include "ui_theme.h"

#if !PANEL_HAS_CAN
#include "espnow_link.h"
#endif

static const char *TAG = "ui";

#define STATUSBAR_H         36
#define IDLE_DIM_TIMEOUT_MS 120000
#define IDLE_OFF_TIMEOUT_MS 300000
#define IDLE_DIM_PERCENT    50

typedef enum {
    BACKLIGHT_NORMAL,
    BACKLIGHT_DIMMED,
    BACKLIGHT_OFF,
} backlight_state_t;

static lv_obj_t *s_buttons[PANEL_BUTTON_COUNT];
static lv_obj_t *s_link_dot;
static lv_obj_t *s_dim_overlay;
static backlight_state_t s_backlight_state;
static bool s_ui_ready;

#if PANEL_HAS_SCREEN_2
static lv_obj_t *s_buttons_2[PANEL_BUTTON_COUNT_2];
static lv_obj_t *s_grid1;
static lv_obj_t *s_grid2;
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
    }
}

/* ------------------------------------------------------- link health ---- */

static void link_health_timer_cb(lv_timer_t *t)
{
    (void)t;
#if PANEL_HAS_CAN
    const bool healthy = state_manager_bus_healthy();
#else
    const bool healthy = espnow_link_healthy();
#endif
    lv_obj_set_style_bg_color(s_link_dot, healthy ? UI_COLOR_OK : UI_COLOR_ERR, 0);
}

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

    s_link_dot = lv_obj_create(bar);
    lv_obj_set_size(s_link_dot, 12, 12);
    lv_obj_set_style_radius(s_link_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_link_dot, 0, 0);
    lv_obj_set_style_bg_color(s_link_dot, UI_COLOR_ERR, 0);
    lv_obj_align(s_link_dot, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_remove_flag(s_link_dot, LV_OBJ_FLAG_SCROLLABLE);

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
    build_button_grid(grid2, PANEL_BUTTONS_2, PANEL_BUTTON_COUNT_2, s_buttons_2);
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
    lv_timer_create(link_health_timer_cb, 500, NULL);
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
