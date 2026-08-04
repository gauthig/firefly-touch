/*
 * board_4_3b — bring-up for the Waveshare ESP32-S3-Touch-LCD-4.3B.
 *
 * Board: ESP32-S3-WROOM-1-N16R8, 4.3" 800x480 RGB565 LCD (ST7262-class
 * panel), GT911 capacitive touch on I2C, CH422G IO expander (touch reset,
 * LCD reset, backlight enable, USB select), TJA1051 CAN transceiver, 7-36 V
 * supply.
 *
 * Pin assignments below are taken from Waveshare's published
 * ESP32-S3-Touch-LCD-4.3 / 4.3B demo code (RGB timing, CH422G init, GT911
 * config). Do NOT re-derive them; when merging updates, diff against the
 * Waveshare demo, not intuition.
 *
 * =====================================================================
 * TODO(critical, before first flash): verify BOARD_TWAI_TX_GPIO /
 * BOARD_TWAI_RX_GPIO against the Waveshare 4.3B *schematic*. The values
 * below (GPIO15/16) are the commonly documented CAN pins for this board
 * family but have NOT been confirmed against the B-variant schematic.
 * A wrong assignment here can disturb the coach's live RV-C bus.
 * =====================================================================
 */
#pragma once

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- LCD geometry ------------------------------------------------------ */
#define BOARD_LCD_H_RES   800
#define BOARD_LCD_V_RES   480

/* ---- RGB LCD pins (from Waveshare ESP32-S3-Touch-LCD-4.3 demo) --------- */
#define BOARD_LCD_GPIO_DE      5
#define BOARD_LCD_GPIO_VSYNC   3
#define BOARD_LCD_GPIO_HSYNC   46
#define BOARD_LCD_GPIO_PCLK    7
/* Data bus, LSB first: B3..B7, G2..G7, R3..R7 */
#define BOARD_LCD_GPIO_DATA0   14  /* B3 */
#define BOARD_LCD_GPIO_DATA1   38  /* B4 */
#define BOARD_LCD_GPIO_DATA2   18  /* B5 */
#define BOARD_LCD_GPIO_DATA3   17  /* B6 */
#define BOARD_LCD_GPIO_DATA4   10  /* B7 */
#define BOARD_LCD_GPIO_DATA5   39  /* G2 */
#define BOARD_LCD_GPIO_DATA6   0   /* G3 */
#define BOARD_LCD_GPIO_DATA7   45  /* G4 */
#define BOARD_LCD_GPIO_DATA8   48  /* G5 */
#define BOARD_LCD_GPIO_DATA9   47  /* G6 */
#define BOARD_LCD_GPIO_DATA10  21  /* G7 */
#define BOARD_LCD_GPIO_DATA11  1   /* R3 */
#define BOARD_LCD_GPIO_DATA12  2   /* R4 */
#define BOARD_LCD_GPIO_DATA13  42  /* R5 */
#define BOARD_LCD_GPIO_DATA14  41  /* R6 */
#define BOARD_LCD_GPIO_DATA15  40  /* R7 */

/* ---- I2C (GT911 touch + CH422G expander share the bus) ----------------- */
#define BOARD_I2C_GPIO_SDA     8
#define BOARD_I2C_GPIO_SCL     9
#define BOARD_I2C_FREQ_HZ      400000
#define BOARD_TOUCH_GPIO_INT   4

/* ---- CH422G expander pins (EXIOn), from Waveshare demo ----------------- */
#define BOARD_EXIO_TP_RST      1   /* GT911 reset */
#define BOARD_EXIO_DISP        2   /* LCD backlight enable (on/off only!) */
#define BOARD_EXIO_LCD_RST     3
#define BOARD_EXIO_SD_CS       4
#define BOARD_EXIO_USB_SEL     5
/* TODO(bench): verify EXIO mapping against the 4.3B schematic; the values
 * above are from the 4.3 demo and are believed identical on the B variant. */

/* ---- TWAI / CAN (TJA1051) ---------------------------------------------- */
/* TODO(critical): VERIFY against Waveshare 4.3B schematic before first
 * flash — see header comment. */
#define BOARD_TWAI_TX_GPIO     15
#define BOARD_TWAI_RX_GPIO     16
#define BOARD_TWAI_BITRATE_KBPS 250

/* ---- API ---------------------------------------------------------------- */

/*
 * Full display bring-up: I2C bus, CH422G (panel out of reset, backlight on),
 * RGB panel, GT911 touch, esp_lvgl_port (LVGL task pinned to core 1).
 * Call once from app_main before any UI code.
 */
esp_err_t board_display_init(void);

lv_display_t *board_get_display(void);
lv_indev_t   *board_get_touch_indev(void);

/*
 * Backlight control. NOTE: on this board the backlight is gated by CH422G
 * EXIO2, which is a plain on/off line — there is no hardware PWM path to the
 * backlight. True brightness control therefore isn't available; percent is
 * mapped to on (>0) / off (0) here, and intermediate dimming is emulated in
 * the UI layer with a translucent overlay (see ui_common).
 * TODO(bench): probe the 4.3B backlight driver enable pad for a PWM-capable
 * route; if one exists, implement LEDC PWM here and drop the UI overlay.
 */
esp_err_t board_backlight_set_percent(uint8_t percent);

/*
 * Install + start the TWAI driver at 250 kbps, normal mode, accept-all
 * filter (DGN filtering is done in software so sniffer mode sees the whole
 * bus). Does not create any tasks.
 */
esp_err_t board_twai_init(void);

#ifdef __cplusplus
}
#endif
