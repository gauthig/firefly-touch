/*
 * main_sim — LVGL PC simulator entry point.
 *
 * Default: opens a 480x800 SDL window rendering the selected panel with the
 * real UI code; mouse = touch (click, click-and-hold to ramp).
 *
 * Why 480x800 and not the panel's physical 800x480: the firmware runs the
 * display rotated 90 degrees (board_4_3b.c sets sw_rotate + ROTATION_90), so
 * every layout decision in the UI code is made against a LOGICAL resolution
 * of 480x800 -- build_screen() explicitly sizes itself off
 * lv_display_get_vertical_resolution() for exactly this reason. Creating the
 * simulator display at the physical size instead would preview a landscape
 * screen the hardware never shows, which is worse than useless for judging a
 * layout. Matching the logical geometry needs no rotation here at all.
 *
 * `--shot <file.bmp> [screen2]`: headless mode — renders one frame to an
 * in-memory display, saves a BMP screenshot, and exits (used for CI /
 * remote review). With `screen2`, taps the TANK LEVELS / BATTERY STATUS
 * button first and lets real time run forward briefly so the tank-status,
 * battery-status and wave-animation timers all have a chance to fire
 * before the snapshot.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL2/SDL.h>

#include "lvgl.h"
#include "ui.h"

void sim_seed_demo_state(void);

static uint32_t tick_cb(void)
{
    return SDL_GetTicks();
}

/* ------------------------------------------------ headless screenshot -- */

/*
 * Headless rendering target. Deliberately a FULL-size XRGB8888 framebuffer
 * that LVGL draws straight into, rather than lv_snapshot_take() on the
 * active screen: a snapshot only walks one object tree, so it silently
 * misses everything on lv_layer_top() -- the idle-dim overlay and the
 * battery bank's per-pack detail popup both live there and never appeared
 * in captures taken the old way. Rendering the display for real and reading
 * back its framebuffer captures exactly what the panel would show.
 */
#define HEADLESS_W 480
#define HEADLESS_H 800
static uint8_t s_headless_fb[HEADLESS_W * HEADLESS_H * 4];

static void headless_flush_cb(lv_display_t *disp, const lv_area_t *area,
                              uint8_t *px_map)
{
    (void)area;
    (void)px_map;   /* px_map IS s_headless_fb in RENDER_MODE_FULL */
    lv_display_flush_ready(disp);
}

/* `data` is XRGB8888, top row first; BMP rows are bottom-up. */
static int write_bmp24(const char *path, const uint8_t *data,
                       uint32_t w, uint32_t h, uint32_t stride)
{
    const uint32_t row = (w * 3 + 3) & ~3u;
    const uint32_t data_size = row * h;
    const uint32_t file_size = 54 + data_size;

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return -1;
    }

    uint8_t hdr[54] = { 'B', 'M' };
    memcpy(&hdr[2], &file_size, 4);
    hdr[10] = 54;                       /* pixel data offset */
    hdr[14] = 40;                       /* BITMAPINFOHEADER */
    memcpy(&hdr[18], &w, 4);
    memcpy(&hdr[22], &h, 4);
    hdr[26] = 1;                        /* planes */
    hdr[28] = 24;                       /* bpp */
    memcpy(&hdr[34], &data_size, 4);
    fwrite(hdr, 1, sizeof(hdr), f);

    /* XRGB8888 in memory is B,G,R,X -- the same channel order BMP wants. */
    for (int32_t y = (int32_t)h - 1; y >= 0; y--) {
        const uint8_t *src = data + (uint32_t)y * stride;
        uint8_t pad[4] = { 0 };
        for (uint32_t x = 0; x < w; x++) {
            fwrite(&src[x * 4], 1, 3, f);
        }
        fwrite(pad, 1, row - w * 3, f);
    }
    fclose(f);
    return 0;
}

/* Recursively finds a button whose direct child label matches `text` and
 * fires a tap on it -- used to reach screen 2 in headless --shot mode
 * (switch_screen() is internal to ui.c, so this drives the same tap path a
 * real touch would). Returns true if found. */
static bool click_button_labeled(lv_obj_t *obj, const char *text)
{
    uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(obj, i);
        if (lv_obj_check_type(child, &lv_label_class) &&
            strcmp(lv_label_get_text(child), text) == 0) {
            lv_obj_send_event(obj, LV_EVENT_SHORT_CLICKED, NULL);
            return true;
        }
        if (click_button_labeled(child, text)) {
            return true;
        }
    }
    return false;
}

static int run_screenshot(const char *path, bool screen2, bool popup, bool screen3)
{
    lv_display_t *disp = lv_display_create(HEADLESS_W, HEADLESS_H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(disp, s_headless_fb, NULL, sizeof(s_headless_fb),
                           LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, headless_flush_cb);

    ui_init();
    sim_seed_demo_state();

    if (screen2) {
        /* Whichever secondary-screen button this panel actually has. */
        if (!click_button_labeled(lv_screen_active(), "TANK LEVELS") &&
            !click_button_labeled(lv_screen_active(), "BATTERY STATUS") &&
            !click_button_labeled(lv_screen_active(), "BATTERY")) {
            fprintf(stderr, "[sim] no screen-switch button found (no screen 2 on this panel?)\n");
        }
        if (popup) {
            /* Battery bank readout only: tapping it reveals the per-pack
             * detail popup. The BANK label is hidden on screen (the widget
             * carries its own labels), but it still exists, so the same
             * label-driven tap helper reaches it. */
            lv_timer_handler();
            if (!click_button_labeled(lv_screen_active(), "BANK")) {
                fprintf(stderr, "[sim] no BANK widget on this panel's screen 2\n");
            }
        }
    }

    if (screen3 && !click_button_labeled(lv_screen_active(), "SHORE POWER")) {
        fprintf(stderr, "[sim] no SHORE POWER button on this panel\n");
    }

    /* Run real time forward so the tank/battery-status timers (500 ms /
     * 1000 ms) and the wave-gauge animation timer (100 ms) all get to fire
     * at least once, with margin -- 40 * 50 ms = ~2 s. */
    for (int i = 0; i < 40; i++) {
        lv_timer_handler();
        SDL_Delay(50);
    }
    lv_refr_now(disp);

    const int rc = write_bmp24(path, s_headless_fb, HEADLESS_W, HEADLESS_H,
                               HEADLESS_W * 4);
    if (rc != 0) {
        fprintf(stderr, "could not write %s\n", path);
        return 1;
    }
    printf("[sim] wrote %s\n", path);
    return 0;
}

/* -------------------------------------------------------- interactive -- */

static int run_window(void)
{
    lv_display_t *disp = lv_sdl_window_create(480, 800);
    lv_sdl_window_set_title(disp, "firefly-touch simulator");
    lv_sdl_mouse_create();

    ui_init();
    sim_seed_demo_state();

    printf("[sim] click = tap/toggle, click-and-hold = ramp, close window to exit\n");
    while (lv_display_get_next(NULL) != NULL) {   /* window close deletes the display */
        const uint32_t wait = lv_timer_handler();
        SDL_Delay(wait > 10 ? 10 : (wait < 1 ? 1 : wait));
    }
    return 0;
}

int main(int argc, char **argv)
{
    SDL_SetMainReady();
    lv_init();
    lv_tick_set_cb(tick_cb);

    if (argc >= 3 && strcmp(argv[1], "--shot") == 0) {
        const bool screen2 = argc >= 4 && strcmp(argv[3], "screen2") == 0;
        const bool screen3 = argc >= 4 && strcmp(argv[3], "screen3") == 0;
        const bool popup = argc >= 5 && strcmp(argv[4], "popup") == 0;
        if (screen3) {
            return run_screenshot(argv[2], false, false, true);
        }
        return run_screenshot(argv[2], screen2, popup, false);
    }
    return run_window();
}
