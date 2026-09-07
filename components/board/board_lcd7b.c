/*
 * Waveshare ESP32-S3-Touch-LCD-7B (1024x600) bring-up.
 *
 * Near-copy of board_lcd7.c (the non-B 7") rather than a shared
 * implementation with per-board #ifdefs -- board_lcd7.c is flashed and
 * coach-verified, so keeping them separate means churn here can never
 * regress it. But the 7B differs by more than geometry (see board_lcd7b.h):
 *
 *   1. RGB porch/pulse block + 30 MHz pclk -- ESPHome's
 *      WAVESHARE-5-1024X600 timings, bench-verified on a real 7B by
 *      xtux77/waveshare-esp32s3-lcd7b-esphome. NOT the NicoEFI "7B demo"
 *      numbers (still 800x480).
 *   2. IO expander is Waveshare's register-addressed "IO_EXTENSION" at
 *      0x24, not a CH422G (`components/ws_io_expander`). I2C scan of the
 *      board: 0x24 + GT911@0x5D answer, the CH422G's 0x23/0x38 do not.
 *   3. EXIO6 = LCD_VDD_EN, an extra panel-supply enable the non-B lacks;
 *      HIGH before RGB init or the panel stays black. See board_display_init.
 *
 * MERGE POINT: the timing block and the EXIO sequencing below. Diff against
 * xtux77's docs/hardware.md + panel.yaml (the most carefully verified 7B
 * reference) when merging, not the vendor wiki -- it calls this board a
 * CH422G board and omits LCD_VDD_EN.
 */
#include "board_lcd7b.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/i2c_master.h"
#include "driver/twai.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

/* The 7B's IO expander is NOT a CH422G -- it is Waveshare's register-
 * addressed "IO_EXTENSION" chip at 0x24 (confirmed by an I2C scan on the
 * real board: 0x24 and the GT911 at 0x5D answer, the CH422G's 0x23/0x38 do
 * not). Same EXIO pin meanings, different wire protocol. */
#include "ws_io_expander.h"

static const char *TAG = "board_lcd7b";

/* LVGL render-task stack. 8 KiB is enough for the grid / readout screens,
 * but the thermostat widget (3 zone cards of nested flex rows + a mode-picker
 * overlay) is the deepest object tree in the project and blows it during
 * render -- "stack overflow in task taskLVGL", which the task WDT turns into
 * a reboot to PANEL_DEFAULT_SCREEN. main_cabinet carries that widget on its
 * CLIMATE section, so its build overrides this via
 * components/board/CMakeLists.txt (24 KiB, internal -- the 7B has ~125 KiB
 * free internal DRAM and no Bluedroid, so no need for the PSRAM placement the
 * 4.3B panels use). Mirrors board_4_3b.c. */
#ifndef BOARD_LVGL_TASK_STACK
#define BOARD_LVGL_TASK_STACK 8192
#endif

static i2c_master_bus_handle_t s_i2c_bus;
static esp_lcd_panel_handle_t s_lcd_panel;
static esp_lcd_touch_handle_t s_touch;
static lv_display_t *s_lv_display;
static lv_indev_t *s_lv_touch_indev;

/* ---------------------------------------------------------------- I2C -- */

static esp_err_t i2c_bus_init(void)
{
    const i2c_master_bus_config_t cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_I2C_GPIO_SDA,
        .scl_io_num = BOARD_I2C_GPIO_SCL,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, &s_i2c_bus);
}

/* ------------------------------------------------------------ RGB LCD -- */

static esp_err_t rgb_panel_init(void)
{
    const esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        /* 1024x600. These are ESPHome's WAVESHARE-5-1024X600 timings (same
         * panel), bench-verified on a real 7B by the xtux77/
         * waveshare-esp32s3-lcd7b-esphome project: correct colours, no
         * inversion. Frame = (1024+30+145+170) x (600+2+23+12) = 1369x637,
         * so 30 MHz pclk => ~34 Hz. ⚠️ Do NOT use the numbers from the
         * NicoEFI "7B demo" repo -- that fork still carries 800x480 porch
         * values and gives a scrambled / blank panel. If horizontal noise
         * or flicker appears, drop pclk to 26 MHz (also bench-proven). */
        .timings = {
            /* 21 MHz. The wiki / xtux77 use 30, but that is with an ESPHome
             * build that does not carry our BLE + ESP-NOW stack (bigger CPU
             * caches were tried to buy back PSRAM bandwidth and blew the
             * internal-RAM budget -> WiFi malloc failures -> boot loop).
             * With full_refresh (below) the whole 1369x637 frame is
             * re-rendered and pushed every refresh, so the scanout has to
             * share PSRAM with a ~1.7 MB/frame copy plus rodata/instruction
             * fetch. 21 MHz => ~24 Hz refresh, ~155 MB/s scanout, which
             * leaves enough headroom for that to stay solid. 24 Hz is low
             * but a rock-steady 24 beats a shaky 30. */
            .pclk_hz = 21 * 1000 * 1000,
            .h_res = BOARD_LCD_H_RES,
            .v_res = BOARD_LCD_V_RES,
            .hsync_pulse_width = 30,
            .hsync_back_porch = 145,
            .hsync_front_porch = 170,
            .vsync_pulse_width = 2,
            .vsync_back_porch = 23,
            .vsync_front_porch = 12,
            .flags.pclk_active_neg = true,
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = 2,
        /* Bounce buffers in internal RAM smooth PSRAM bandwidth spikes.
         * H_RES*10 px = ~20 KiB each at 1024 wide; the 30 MHz pclk pushes
         * ~2.3x the bandwidth of the non-B, so this is the first knob to
         * raise (H_RES*20) if the panel tears or flickers -- see the
         * board_lcd7b bring-up notes. */
        .bounce_buffer_size_px = BOARD_LCD_H_RES * 10,
        .psram_trans_align = 64,
        .hsync_gpio_num = BOARD_LCD_GPIO_HSYNC,
        .vsync_gpio_num = BOARD_LCD_GPIO_VSYNC,
        .de_gpio_num = BOARD_LCD_GPIO_DE,
        .pclk_gpio_num = BOARD_LCD_GPIO_PCLK,
        .disp_gpio_num = -1,   /* DISP is driven via IO_EXTENSION EXIO2 */
        .data_gpio_nums = {
            BOARD_LCD_GPIO_DATA0,  BOARD_LCD_GPIO_DATA1,  BOARD_LCD_GPIO_DATA2,
            BOARD_LCD_GPIO_DATA3,  BOARD_LCD_GPIO_DATA4,  BOARD_LCD_GPIO_DATA5,
            BOARD_LCD_GPIO_DATA6,  BOARD_LCD_GPIO_DATA7,  BOARD_LCD_GPIO_DATA8,
            BOARD_LCD_GPIO_DATA9,  BOARD_LCD_GPIO_DATA10, BOARD_LCD_GPIO_DATA11,
            BOARD_LCD_GPIO_DATA12, BOARD_LCD_GPIO_DATA13, BOARD_LCD_GPIO_DATA14,
            BOARD_LCD_GPIO_DATA15,
        },
        .flags.fb_in_psram = true,
    };

    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&cfg, &s_lcd_panel), TAG, "new rgb panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_lcd_panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_lcd_panel), TAG, "panel init");
    return ESP_OK;
}

/* -------------------------------------------------------------- Touch -- */

static esp_err_t touch_init(void)
{
    /* GT911 reset is wired through the IO expander. Address selection: INT level
     * during reset picks 0x5D vs 0x14; with INT left as input this lands on
     * the default 0x5D.
     * TODO(bench): if the GT911 doesn't ACK at 0x5D, set io_cfg.dev_addr =
     * ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP (0x14). */
    ESP_RETURN_ON_ERROR(ws_io_expander_set_pin(BOARD_EXIO_TP_RST, false), TAG, "tp rst low");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(ws_io_expander_set_pin(BOARD_EXIO_TP_RST, true), TAG, "tp rst high");
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_cfg.scl_speed_hz = BOARD_I2C_FREQ_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c_bus, &io_cfg, &io),
                        TAG, "gt911 panel io");

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BOARD_LCD_H_RES,
        .y_max = BOARD_LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,   /* handled via IO expander above */
        .int_gpio_num = BOARD_TOUCH_GPIO_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    return esp_lcd_touch_new_i2c_gt911(io, &tp_cfg, &s_touch);
}

/* --------------------------------------------------------- LVGL glue -- */

static esp_err_t lvgl_init(void)
{
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 4;
    port_cfg.task_stack = BOARD_LVGL_TASK_STACK;
    port_cfg.task_affinity = 1;   /* UI on core 1; protocol tasks own core 0 */
#ifdef BOARD_LVGL_TASK_STACK_PSRAM
    /* PSRAM stack: SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY + TASK_CREATE_ALLOW_EXT_MEM
     * are on, and the LVGL task never runs in ISR context, so this is safe. */
    port_cfg.task_stack_caps = MALLOC_CAP_SPIRAM;
#endif
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl_port_init");

    const lvgl_port_display_cfg_t disp_cfg = {
        .panel_handle = s_lcd_panel,
        .buffer_size = BOARD_LCD_H_RES * BOARD_LCD_V_RES,
        .double_buffer = true,
        .hres = BOARD_LCD_H_RES,
        .vres = BOARD_LCD_V_RES,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
            .swap_bytes = false,
            /* Landscape, native orientation -- no sw_rotate (unlike
             * board_4_3b.c, which rotates 90 deg to portrait).
             *
             * full_refresh + avoid_tearing (rgb_cfg below): the RGB buffer
             * scheme, bench-fought 2026-09-05. board_lcd7.c's partial +
             * bounce blinked on the 1024x600 panel (RGB DMA ran dry between
             * partial flushes). avoid_tearing hands esp_lvgl_port the RGB
             * panel's two PSRAM framebuffers as LVGL's draw buffers and
             * swaps them on vsync (via disp_ctx->trans_sem) -- so a whole
             * frame is always ready to scan out AND no separate LVGL draw
             * buffers are allocated (~2.3 MiB PSRAM saved).
             *
             * full_refresh, NOT direct_mode: direct_mode kept static
             * content solid but every widget that redraws (the 1 Hz
             * readouts, the tank-wave animation) flickered -- the two
             * alternating buffers briefly disagree in the redrawn region.
             * full_refresh re-renders the WHOLE frame into the back buffer
             * every refresh, so the buffers never disagree -> no flicker.
             * It only holds if render+push keeps ahead of the scanout,
             * which is why pclk is 21 MHz (see the timings block).
             * CONFIG_LCD_RGB_RESTART_IN_VSYNC made it worse (wobble) -- left
             * off. Safe without avoid_tearing's usual sw_rotate conflict
             * because this board doesn't rotate. */
            .full_refresh = true,
        },
    };
    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {
            .bb_mode = false,
            .avoid_tearing = true,
        },
    };
    s_lv_display = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);
    ESP_RETURN_ON_FALSE(s_lv_display != NULL, ESP_FAIL, TAG, "add display");

    /* No lv_display_set_rotation() call: 1024x600 landscape is both the
     * physical and the logical resolution on this board. */

    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = s_lv_display,
        .handle = s_touch,
    };
    s_lv_touch_indev = lvgl_port_add_touch(&touch_cfg);
    ESP_RETURN_ON_FALSE(s_lv_touch_indev != NULL, ESP_FAIL, TAG, "add touch");
    return ESP_OK;
}

/* ---------------------------------------------------------------- API -- */

esp_err_t board_display_init(void)
{
    ESP_RETURN_ON_ERROR(i2c_bus_init(), TAG, "i2c");
    ESP_RETURN_ON_ERROR(ws_io_expander_init(s_i2c_bus), TAG, "io expander");

    /* ⚠️ 7B-only: bring up the panel's VCOM supply (EXIO6 = LCD_VDD_EN)
     * BEFORE reset / RGB init. Without this the backlight lights but the
     * panel never shows an image -- the "double enable pin" of the 7B. */
    ESP_RETURN_ON_ERROR(ws_io_expander_set_pin(BOARD_EXIO_LCD_VDD_EN, true), TAG, "lcd vdd en");
    vTaskDelay(pdMS_TO_TICKS(20));

    /* Panel reset. */
    ESP_RETURN_ON_ERROR(ws_io_expander_set_pin(BOARD_EXIO_LCD_RST, false), TAG, "lcd rst low");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(ws_io_expander_set_pin(BOARD_EXIO_LCD_RST, true), TAG, "lcd rst high");
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_RETURN_ON_ERROR(rgb_panel_init(), TAG, "rgb panel");
    ESP_RETURN_ON_ERROR(touch_init(), TAG, "touch");
    ESP_RETURN_ON_ERROR(lvgl_init(), TAG, "lvgl");

    ESP_RETURN_ON_ERROR(board_backlight_set_percent(100), TAG, "backlight");

    /* Recorded so the bench flash captures the real headroom on the 7B --
     * a 1024x600 framebuffer set is ~2.3x the non-B's. docs/SYSTEM.md's
     * memory-budget table wants these numbers. */
    ESP_LOGI(TAG, "heap free after display init: internal %u B, PSRAM %u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "display up: 1024x600 RGB565 landscape, GT911 touch, LVGL on core 1");
    return ESP_OK;
}

lv_display_t *board_get_display(void)
{
    return s_lv_display;
}

lv_indev_t *board_get_touch_indev(void)
{
    return s_lv_touch_indev;
}

esp_err_t board_backlight_set_percent(uint8_t percent)
{
    /* IO2 is a plain output bit, matching board_lcd7.c. The chip CAN do real
     * PWM (ws_io_expander_set_backlight_pct); wiring that into the idle-dim
     * path is a future improvement, kept on/off here for parity.
     *
     * ⚠️ On the 7B, EXIO2 also gates the panel's DISP line, so percent==0
     * here puts the LCD into standby (xtux77 trap #5) rather than a clean
     * blank. The UI's opaque black idle overlay covers it; re-verify the
     * 300 s idle-off stage on hardware. */
    return ws_io_expander_set_pin(BOARD_EXIO_DISP, percent > 0);
}

esp_err_t board_twai_init(void)
{
    /* GPIO19/20 are shared between native USB and the CAN transceiver;
     * EXIO5 picks which (IO_EXTENSION bit 5). Raising it here is what makes CAN work at all --
     * and it is also what makes the native USB port go dark, so this board
     * is flashed and monitored over UART (CH343). */
    ESP_RETURN_ON_ERROR(ws_io_expander_set_pin(BOARD_EXIO_USB_SEL, BOARD_USB_SEL_CAN_LEVEL),
                        TAG, "usb/can mux");
    ESP_LOGW(TAG, "EXIO5 -> CAN: native USB port is now disabled, use UART");

    twai_general_config_t g_cfg = TWAI_GENERAL_CONFIG_DEFAULT(
        BOARD_TWAI_TX_GPIO, BOARD_TWAI_RX_GPIO, TWAI_MODE_NORMAL);
    g_cfg.rx_queue_len = 32;
    g_cfg.tx_queue_len = 8;

    /* RV-C runs at 250 kbps. */
    const twai_timing_config_t t_cfg = TWAI_TIMING_CONFIG_250KBITS();
    /* Accept everything: DGN filtering happens in software so the sniffer
     * build can log the whole bus. */
    const twai_filter_config_t f_cfg = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    ESP_RETURN_ON_ERROR(twai_driver_install(&g_cfg, &t_cfg, &f_cfg), TAG, "twai install");
    ESP_RETURN_ON_ERROR(twai_start(), TAG, "twai start");
    ESP_LOGI(TAG, "TWAI up at 250 kbps on TX=%d RX=%d",
             BOARD_TWAI_TX_GPIO, BOARD_TWAI_RX_GPIO);
    return ESP_OK;
}
