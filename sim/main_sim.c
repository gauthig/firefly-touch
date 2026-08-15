/*
 * main_sim — LVGL PC simulator entry point.
 *
 * Default: opens an 800x480 SDL window rendering the selected panel with the
 * real UI code; mouse = touch (click, click-and-hold to ramp).
 *
 * `--shot <file.bmp> [screen2]`: headless mode — renders one frame to an
 * in-memory display, saves a BMP screenshot, and exits (used for CI /
 * remote review). With `screen2`, taps the TANK LEVELS button first and
 * lets real time run forward briefly so the tank-status timer and wave
 * animation have a chance to fire before the snapshot.
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

static void headless_flush_cb(lv_display_t *disp, const lv_area_t *area,
                              uint8_t *px_map)
{
    (void)area;
    (void)px_map;
    lv_display_flush_ready(disp);
}

static int write_bmp24(const char *path, const lv_draw_buf_t *buf)
{
    const uint32_t w = buf->header.w;
    const uint32_t h = buf->header.h;
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

    /* Snapshot is XRGB8888: bytes B,G,R,X. BMP rows are bottom-up. */
    for (int32_t y = (int32_t)h - 1; y >= 0; y--) {
        const uint8_t *src = buf->data + (uint32_t)y * buf->header.stride;
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

static int run_screenshot(const char *path, bool screen2)
{
    static lv_color_t draw_buf[800 * 60];
    lv_display_t *disp = lv_display_create(800, 480);
    lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, headless_flush_cb);

    ui_init();
    sim_seed_demo_state();

    if (screen2) {
        if (!click_button_labeled(lv_screen_active(), "TANK LEVELS")) {
            fprintf(stderr, "[sim] TANK LEVELS button not found (no screen 2 on this panel?)\n");
        }
    }

    /* Run real time forward so the tank-status timer (500 ms) and the
     * wave-gauge animation timer (100 ms) both get to fire at least once. */
    for (int i = 0; i < 20; i++) {
        lv_timer_handler();
        SDL_Delay(50);
    }
    lv_refr_now(disp);

    lv_draw_buf_t *snap = lv_snapshot_take(lv_screen_active(),
                                           LV_COLOR_FORMAT_XRGB8888);
    if (snap == NULL) {
        fprintf(stderr, "snapshot failed\n");
        return 1;
    }
    const int rc = write_bmp24(path, snap);
    lv_draw_buf_destroy(snap);
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
    lv_display_t *disp = lv_sdl_window_create(800, 480);
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
        return run_screenshot(argv[2], screen2);
    }
    return run_window();
}
