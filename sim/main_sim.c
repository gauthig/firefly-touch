/*
 * main_sim — LVGL PC simulator entry point.
 *
 * Default: opens an SDL window rendering the selected panel with the real UI
 * code; mouse = touch (click, click-and-hold to ramp).
 *
 * WINDOW SIZE MATCHES THE PANEL'S LOGICAL RESOLUTION, WHICH IS NOT ALWAYS
 * ITS PHYSICAL ONE:
 *
 *   - 4.3B panels are physically 800x480 but run rotated 90 degrees
 *     (board_4_3b.c sets sw_rotate + ROTATION_90), so all layout code sees
 *     480x800 portrait. build_screen() sizes itself off
 *     lv_display_get_vertical_resolution() for exactly this reason.
 *   - The Waveshare 7" (main_cabinet) runs unrotated, so its logical
 *     resolution IS 800x480 landscape -- which is what makes room for the
 *     side-nav rail.
 *
 * Previewing the wrong one shows a screen the hardware never displays, which
 * is worse than useless for judging a layout. PANEL_HAS_NAV_RAIL is the
 * marker for the landscape case; matching the logical geometry means no
 * rotation is needed on the sim side either way.
 *
 * `--shot <file.bmp> [screen2|screen3|section:<LABEL>]`: headless mode —
 * renders one frame to an in-memory display, saves a BMP screenshot, and
 * exits (used for CI / remote review). Any of those taps a section button
 * first, then lets real time run forward briefly so the tank-status,
 * battery-status and wave-animation timers all have a chance to fire before
 * the snapshot. `section:` names the button explicitly, which is what a
 * side-nav panel needs -- it has more sections than "screen 2 / screen 3"
 * can describe, e.g. `section:LIGHTS`.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>

#include "lvgl.h"
#include "panel_config.h"
#include "ui.h"

/* Logical geometry of the panel being previewed -- see the file header. */
#if PANEL_HAS_NAV_RAIL
#define SIM_W 800
#define SIM_H 480
#else
#define SIM_W 480
#define SIM_H 800
#endif

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
#define HEADLESS_W SIM_W
#define HEADLESS_H SIM_H
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

/*
 * Scripted pointer input device.
 *
 * Sending LV_EVENT_SHORT_CLICKED straight to a widget (which this harness
 * used to do) exercises the handler but NOT LVGL's input plumbing -- no
 * hit-testing, no indev state machine, no press/release lifecycle. A whole
 * class of bug lives in exactly that gap: the battery pack-detail popup
 * opened once and then refused to reopen on real hardware, while scripted
 * events toggled it perfectly. Driving a real indev reproduces it.
 */
static lv_point_t s_ptr;
static bool       s_ptr_pressed;

static void scripted_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point = s_ptr;
    data->state = s_ptr_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void pump(int iterations)
{
    for (int i = 0; i < iterations; i++) {
        lv_timer_handler();
        SDL_Delay(20);
    }
}

/*
 * A real press-and-release at (x, y). Default hold is well under LVGL's
 * 400 ms long-press threshold, so it registers as a short click.
 *
 * SIM_TAP_HOLD_MS overrides the hold time -- set it above 400 to emulate a
 * deliberate finger press, which is a genuinely different code path:
 * LV_EVENT_SHORT_CLICKED is NOT sent once a press crosses the long-press
 * threshold, only LV_EVENT_CLICKED. Any widget that listens solely for
 * SHORT_CLICKED silently ignores a slow tap.
 */
static void tap_at(int32_t x, int32_t y)
{
    static int hold_ms = -1;
    if (hold_ms < 0) {
        const char *env = getenv("SIM_TAP_HOLD_MS");
        hold_ms = (env != NULL) ? atoi(env) : 80;
    }

    s_ptr.x = x;
    s_ptr.y = y;
    s_ptr_pressed = true;
    pump((hold_ms / 20) + 1);
    s_ptr_pressed = false;
    pump(6);
}

/* Recursively finds the widget whose direct child label matches `text`. */
static lv_obj_t *find_button_labeled(lv_obj_t *obj, const char *text)
{
    uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(obj, i);
        if (lv_obj_check_type(child, &lv_label_class) &&
            strcmp(lv_label_get_text(child), text) == 0) {
            return obj;
        }
        lv_obj_t *found = find_button_labeled(child, text);
        if (found != NULL) {
            return found;
        }
    }
    return NULL;
}

/* Taps the centre of the widget carrying `text`, through the real indev. */
static bool click_button_labeled(lv_obj_t *root, const char *text)
{
    lv_obj_t *target = find_button_labeled(root, text);
    if (target == NULL) {
        return false;
    }
    lv_obj_update_layout(target);
    lv_area_t a;
    lv_obj_get_coords(target, &a);
    tap_at((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
    return true;
}

static int run_screenshot(const char *path, bool screen2, int popup_taps, bool screen3,
                          const char *section)
{
    lv_display_t *disp = lv_display_create(HEADLESS_W, HEADLESS_H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(disp, s_headless_fb, NULL, sizeof(s_headless_fb),
                           LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, headless_flush_cb);

    /* Real input device, so scripted taps go through hit-testing and the
     * indev state machine exactly as a finger would. */
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, scripted_read_cb);
    lv_indev_set_display(indev, disp);

    ui_init();
    sim_seed_demo_state();

    if (section != NULL) {
        /* Explicit section name(s) -- the only way to reach an arbitrary one
         * on a side-nav panel, which has more than the two the flags below
         * assume (and whose home screen is not necessarily the grid).
         *
         * Comma-separated for a SEQUENCE of taps: reaching a control inside
         * a section means opening that section first, since a hidden widget
         * cannot be hit-tested. e.g. "TANKS,GREY CLOSED". */
        char buf[128];
        snprintf(buf, sizeof(buf), "%s", section);
        for (char *label = strtok(buf, ","); label != NULL;
             label = strtok(NULL, ",")) {
            lv_timer_handler();   /* let the previous tap's screen appear */
            if (!click_button_labeled(lv_screen_active(), label)) {
                fprintf(stderr, "[sim] no button labeled \"%s\" on this panel\n",
                        label);
            }
        }
    }

    if (screen2) {
        /* Whichever secondary-screen button the built panel actually has.
         * A side-nav panel names its sections on the rail instead. */
        if (!click_button_labeled(lv_screen_active(), "TANK LEVELS") &&
            !click_button_labeled(lv_screen_active(), "TANKS") &&
            !click_button_labeled(lv_screen_active(), "BATTERY STATUS") &&
            !click_button_labeled(lv_screen_active(), "BATTERY")) {
            fprintf(stderr, "[sim] no screen-switch button found (no screen 2 on this panel?)\n");
        }
        if (popup_taps > 0) {
            /* Battery bank readout only: tapping it reveals the per-pack
             * detail popup. The BANK label is hidden on screen (the widget
             * carries its own labels), but it still exists, so the same
             * label-driven tap helper reaches it.
             *
             * Repeating the tap is how the show/hide TOGGLE gets tested --
             * an odd count should leave the popup visible, an even count
             * hidden. */
            for (int tap = 0; tap < popup_taps; tap++) {
                lv_timer_handler();
                if (!click_button_labeled(lv_screen_active(), "BANK")) {
                    fprintf(stderr, "[sim] no BANK widget on this panel's screen 2\n");
                    break;
                }
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
    lv_display_t *disp = lv_sdl_window_create(SIM_W, SIM_H);
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
        /* `popup [n]` taps the bank readout n times (default 1) so the
         * show/hide toggle can be exercised, not just the first reveal. */
        int popup_taps = 0;
        if (argc >= 5 && strcmp(argv[4], "popup") == 0) {
            popup_taps = (argc >= 6) ? atoi(argv[5]) : 1;
        }
        /* `section:<LABEL>` taps a button by name -- needed for a side-nav
         * panel, where "screen 2" is not a meaningful description of a
         * section reachable from a persistent rail. */
        const char *section = NULL;
        if (argc >= 4 && strncmp(argv[3], "section:", 8) == 0) {
            section = argv[3] + 8;
        }
        if (screen3) {
            return run_screenshot(argv[2], false, 0, true, NULL);
        }
        return run_screenshot(argv[2], screen2, popup_taps, false, section);
    }
    return run_window();
}
