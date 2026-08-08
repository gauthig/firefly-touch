/*
 * ui — panel screen: status bar, 2x4 button grid, CAN-health dot,
 * idle auto-dim. Runs in the LVGL task on core 1.
 *
 * Touch never talks to TWAI directly: button callbacks post to the TX queue
 * via twai_enqueue_dimmer_cmd(). Visual state is driven only by
 * ui_on_status(), i.e. by DC_DIMMER_STATUS_3 frames from the bus.
 */
#include "ui.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "board_4_3b.h"
#include "panel_config.h"
#include "state_manager.h"
#include "twai_tasks.h"
#include "ui_dimmer_button.h"
#include "ui_theme.h"

static const char *TAG = "ui";

#define STATUSBAR_H        36
#define IDLE_DIM_TIMEOUT_MS 300000
#define IDLE_DIM_PERCENT    20

static lv_obj_t *s_buttons[PANEL_BUTTON_COUNT];
static lv_obj_t *s_can_dot;
static lv_obj_t *s_dim_overlay;
static uint8_t s_user_backlight_pct = 100;
static bool s_auto_dimmed;
static bool s_ui_ready;

/* ------------------------------------------------------- backlight ------ */

/*
 * The 4.3B backlight enable (CH422G EXIO2) is on/off only, so intermediate
 * brightness is emulated with a translucent black overlay on the LVGL top
 * layer. When the hardware PWM TODO in board_4_3b.h is resolved, route
 * percent to board_backlight_set_percent() and drop the overlay opacity.
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
     * underneath never fires. The overlay is only CLICKABLE while
     * auto-dimmed; at manual partial brightness touches pass through. */
    if (lv_event_get_code(e) == LV_EVENT_PRESSED && s_auto_dimmed) {
        s_auto_dimmed = false;
        lv_obj_remove_flag(s_dim_overlay, LV_OBJ_FLAG_CLICKABLE);
        apply_backlight(s_user_backlight_pct);
    }
}

static void idle_timer_cb(lv_timer_t *t)
{
    (void)t;
    uint32_t inactive_ms = lv_display_get_inactive_time(NULL);
    /* TEMP DIAGNOSTIC: remove once the idle-dim bug is root-caused. If this
     * value keeps resetting to ~0 instead of climbing, something (most
     * likely a stuck/phantom touch read) is continuously registering as
     * activity and the idle timer will never fire. */
    ESP_LOGI(TAG, "idle: inactive_ms=%lu auto_dimmed=%d",
             (unsigned long)inactive_ms, (int)s_auto_dimmed);
    if (!s_auto_dimmed && inactive_ms > IDLE_DIM_TIMEOUT_MS) {
        s_auto_dimmed = true;
        apply_backlight(IDLE_DIM_PERCENT);
        lv_obj_add_flag(s_dim_overlay, LV_OBJ_FLAG_CLICKABLE);
    }
}

/* ------------------------------------------------------- CAN health ----- */

static void can_health_timer_cb(lv_timer_t *t)
{
    (void)t;
    lv_obj_set_style_bg_color(
        s_can_dot,
        state_manager_bus_healthy() ? UI_COLOR_OK : UI_COLOR_ERR, 0);
}

/* ------------------------------------------------------- commands ------- */

static void cycle_local_backlight(void)
{
    /* PANEL LIGHTS also drives our own LCD: cycle 100 -> 60 -> 20 -> 100. */
    s_user_backlight_pct = (s_user_backlight_pct > 60) ? 60
                         : (s_user_backlight_pct > 20) ? 20
                         : 100;
    if (!s_auto_dimmed) {
        apply_backlight(s_user_backlight_pct);
    }
    ESP_LOGI(TAG, "local backlight -> %u%%", s_user_backlight_pct);
}

static void panel_send_cb(const panel_btn_def_t *def, rvc_dimmer_cmd_t cmd,
                          void *user_ctx)
{
    (void)user_ctx;

    if (def->type == PANEL_BTN_PANEL_LIGHTS) {
        /* TODO(bench): capture the factory panel's PL1 frames in sniffer
         * mode and transmit whatever DGN the factory switch backlights
         * actually use. For now this only logs and dims our own LCD. */
        ESP_LOGI(TAG, "PANEL LIGHTS pressed (PL1 DGN unknown — sniff to implement)");
        cycle_local_backlight();
        return;
    }

    for (uint8_t i = 0; i < def->instance_count; i++) {
        twai_enqueue_dimmer_cmd(def->instances[i], cmd,
                                RVC_FIELD_NA, RVC_FIELD_NA);
    }
}

/* ------------------------------------------------------- screen build --- */

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

    s_can_dot = lv_obj_create(bar);
    lv_obj_set_size(s_can_dot, 12, 12);
    lv_obj_set_style_radius(s_can_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_can_dot, 0, 0);
    lv_obj_set_style_bg_color(s_can_dot, UI_COLOR_ERR, 0);
    lv_obj_align(s_can_dot, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_remove_flag(s_can_dot, LV_OBJ_FLAG_SCROLLABLE);

    /* --- 2 x 4 button grid --- */
    static int32_t col_dsc[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
    static int32_t row_dsc[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
                                 LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };

    lv_obj_t *grid = lv_obj_create(scr);
    lv_obj_add_style(grid, &ui_style_screen, 0);
    /* Use logical height (post-rotation) so the grid fills correctly in
     * both landscape (480 px) and portrait (800 px) orientations. */
    int32_t logical_h = (int32_t)lv_display_get_vertical_resolution(NULL);
    lv_obj_set_size(grid, LV_PCT(100), logical_h - STATUSBAR_H);
    lv_obj_align(grid, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_grid_dsc_array(grid, col_dsc, row_dsc);
    lv_obj_set_style_pad_all(grid, 3, 0);
    lv_obj_set_style_pad_column(grid, 3, 0);
    lv_obj_set_style_pad_row(grid, 3, 0);

    for (uint32_t i = 0; i < PANEL_BUTTON_COUNT; i++) {
        lv_obj_t *btn = ui_dimmer_button_create(grid, &PANEL_BUTTONS[i],
                                                panel_send_cb, NULL);
        lv_obj_set_grid_cell(btn,
                             LV_GRID_ALIGN_STRETCH, i % 2, 1,
                             LV_GRID_ALIGN_STRETCH, i / 2, 1);
        s_buttons[i] = btn;
    }

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
    lv_timer_create(can_health_timer_cb, 500, NULL);
}

/* ------------------------------------------------------------- API ------ */

void ui_init(void)
{
    lvgl_port_lock(0);
    build_screen();
    lvgl_port_unlock();
    s_ui_ready = true;
    ESP_LOGI(TAG, "UI ready: %s (%u buttons)", PANEL_NAME,
             (unsigned)PANEL_BUTTON_COUNT);
}

void ui_on_status(uint8_t instance, uint8_t level, bool on)
{
    if (!s_ui_ready) {
        return;
    }
    lvgl_port_lock(0);
    for (uint32_t i = 0; i < PANEL_BUTTON_COUNT; i++) {
        ui_dimmer_button_update(s_buttons[i], instance, level, on);
    }
    lvgl_port_unlock();
}
