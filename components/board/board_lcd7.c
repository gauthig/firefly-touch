/*
 * Waveshare ESP32-S3-Touch-LCD-7 (non-B) bring-up.
 *
 * ============================ MERGE POINT ============================
 * The RGB timing block, CH422G sequencing and GT911 setup below follow
 * Espressif's ESP32_Display_Panel board definition for this board
 * (src/board/supported/waveshare/BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_7.h)
 * and Waveshare's own demo. When bringing up hardware, diff against those
 * — especially:
 *   - esp_lcd_rgb_timing_t values (pclk, porches, pulse widths)
 *   - CH422G EXIO assignments and reset sequencing
 *   - GT911 I2C address selection (0x5D vs 0x14 depends on INT strap
 *     level during reset)
 * =====================================================================
 *
 * This file is deliberately a near-copy of board_4_3b.c rather than a
 * shared implementation with per-board #ifdefs: the two boards differ in
 * CAN pins, a USB/CAN mux and screen orientation, and the 4.3B version is
 * flashed and working on three installed panels. Keeping them separate
 * means bring-up churn here can never regress those.
 */
#include "board_lcd7.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/i2c_master.h"
#include "driver/twai.h"
#include "esp_check.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

#include "ch422g.h"

static const char *TAG = "board_lcd7";

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
        /* EK9716 800x480, from Espressif's board definition. Identical to
         * the 4.3" values except the vertical porches, which are 8/8 here
         * and 16/16 there. Confirmed on real hardware 2026-08-23: correct
         * geometry and colours, no tearing or roll. MERGE POINT: diff
         * against the current upstream board definition when merging. */
        .timings = {
            .pclk_hz = 16 * 1000 * 1000,
            .h_res = BOARD_LCD_H_RES,
            .v_res = BOARD_LCD_V_RES,
            .hsync_pulse_width = 4,
            .hsync_back_porch = 8,
            .hsync_front_porch = 8,
            .vsync_pulse_width = 4,
            .vsync_back_porch = 8,
            .vsync_front_porch = 8,
            .flags.pclk_active_neg = true,
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = 2,
        /* Bounce buffers in internal RAM smooth PSRAM bandwidth spikes. */
        .bounce_buffer_size_px = BOARD_LCD_H_RES * 10,
        .psram_trans_align = 64,
        .hsync_gpio_num = BOARD_LCD_GPIO_HSYNC,
        .vsync_gpio_num = BOARD_LCD_GPIO_VSYNC,
        .de_gpio_num = BOARD_LCD_GPIO_DE,
        .pclk_gpio_num = BOARD_LCD_GPIO_PCLK,
        .disp_gpio_num = -1,   /* DISP is driven via CH422G EXIO2 */
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
    /* GT911 reset is wired through the CH422G. Address selection: INT level
     * during reset picks 0x5D vs 0x14; with INT left as input this lands on
     * the default 0x5D.
     * TODO(bench): if the GT911 doesn't ACK at 0x5D, set io_cfg.dev_addr =
     * ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP (0x14). */
    ESP_RETURN_ON_ERROR(ch422g_set_pin(BOARD_EXIO_TP_RST, false), TAG, "tp rst low");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(ch422g_set_pin(BOARD_EXIO_TP_RST, true), TAG, "tp rst high");
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_cfg.scl_speed_hz = BOARD_I2C_FREQ_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c_bus, &io_cfg, &io),
                        TAG, "gt911 panel io");

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BOARD_LCD_H_RES,
        .y_max = BOARD_LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,   /* handled via CH422G above */
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
    port_cfg.task_stack = 8192;
    port_cfg.task_affinity = 1;   /* UI on core 1; protocol tasks own core 0 */
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
            /* Landscape: this panel is used in its native orientation, so
             * there is no sw_rotate here (unlike board_4_3b.c, which
             * rotates 90 degrees to portrait).
             *
             * Do NOT set full_refresh: in esp_lvgl_port's lvgl9 RGB flush
             * path it waits on disp_ctx->trans_sem, which is only created
             * when avoid_tearing is true, so the two must be changed
             * together or not at all. Partial-render mode (both flags
             * unset) is what the 4.3B panels run in, and bb_mode below
             * already gives tear-free RGB output. */
        },
    };
    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {
            .bb_mode = true,
            .avoid_tearing = false,
        },
    };
    s_lv_display = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);
    ESP_RETURN_ON_FALSE(s_lv_display != NULL, ESP_FAIL, TAG, "add display");

    /* No lv_display_set_rotation() call: 800x480 landscape is both the
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
    ESP_RETURN_ON_ERROR(ch422g_init(s_i2c_bus), TAG, "ch422g");

    /* Panel reset + backlight sequencing (per Waveshare demo). */
    ESP_RETURN_ON_ERROR(ch422g_set_pin(BOARD_EXIO_LCD_RST, false), TAG, "lcd rst low");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(ch422g_set_pin(BOARD_EXIO_LCD_RST, true), TAG, "lcd rst high");
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_RETURN_ON_ERROR(rgb_panel_init(), TAG, "rgb panel");
    ESP_RETURN_ON_ERROR(touch_init(), TAG, "touch");
    ESP_RETURN_ON_ERROR(lvgl_init(), TAG, "lvgl");

    ESP_RETURN_ON_ERROR(board_backlight_set_percent(100), TAG, "backlight");
    ESP_LOGI(TAG, "display up: 800x480 RGB565 landscape, GT911 touch, LVGL on core 1");
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
    /* CH422G EXIO2 is on/off only — see header for the PWM limitation. */
    return ch422g_set_pin(BOARD_EXIO_DISP, percent > 0);
}

esp_err_t board_twai_init(void)
{
    /* GPIO19/20 are shared between native USB and the CAN transceiver;
     * EXIO5 picks which. Raising it here is what makes CAN work at all —
     * and it is also what makes the native USB port go dark, so this board
     * is flashed and monitored over UART. */
    ESP_RETURN_ON_ERROR(ch422g_set_pin(BOARD_EXIO_USB_SEL, BOARD_USB_SEL_CAN_LEVEL),
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
