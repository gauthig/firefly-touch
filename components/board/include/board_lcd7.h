/*
 * board_lcd7 — bring-up for the Waveshare ESP32-S3-Touch-LCD-7 (non-B).
 *
 * Board: ESP32-S3-WROOM-1, 7" 800x480 RGB565 LCD (EK9716 panel), GT911
 * capacitive touch on I2C, CH422G IO expander, TJA1051 CAN transceiver,
 * RS485 transceiver, 7-36 V supply.
 *
 * Verified on the real board over COM19 (esptool flash_id): ESP32-S3
 * rev v0.2, 16 MB flash, 8 MB PSRAM. That matches the 4.3B, so this board
 * shares partitions.csv and sdkconfig.defaults unchanged — despite the
 * Waveshare wiki listing 8 MB flash for the product line.
 *
 * Differences from board_4_3b, and the reasons they matter:
 *
 *   - CAN is on GPIO20 (TX) / GPIO19 (RX). On the 4.3B those pins are
 *     15/16, and on THIS board 15/16 drive the RS485 transceiver instead.
 *     A 4.3B-configured binary flashed here talks to RS485 and the CAN bus
 *     stays silent with no error anywhere.
 *
 *   - GPIO19/20 are also the native USB D-/D+ lines. CH422G EXIO5 muxes
 *     between them: low = USB, high = CAN. board_twai_init() raises EXIO5,
 *     which means THE NATIVE USB PORT STOPS WORKING once CAN is up — flash
 *     and monitor this board over its UART port.
 *
 *   - Landscape, no rotation. The 4.3B panels run rotated 90 degrees to
 *     portrait; this one is used in its native orientation so the side-nav
 *     rail has width to live in.
 *
 * ============================ MERGE POINT ============================
 * RGB timings below are taken from Espressif's own board definition,
 * ESP32_Display_Panel/src/board/supported/waveshare/
 * BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_7.h. They are identical to the 4.3"
 * values EXCEPT the vertical porches (8/8 here vs 16/16 there). Diff
 * against that file — or the current Waveshare demo — when merging
 * upstream changes. Do not re-derive them by intuition.
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

/* ---- RGB LCD pins (identical to the 4.3B) ------------------------------ */
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

/* ---- CH422G expander pins (EXIOn) -------------------------------------- */
#define BOARD_EXIO_TP_RST      1   /* GT911 reset */
#define BOARD_EXIO_DISP        2   /* LCD backlight enable (on/off only!) */
#define BOARD_EXIO_LCD_RST     3
#define BOARD_EXIO_SD_CS       4
#define BOARD_EXIO_USB_SEL     5   /* low = native USB, high = CAN */

/* USB_SEL level that routes GPIO19/20 to the CAN transceiver.
 * Polarity came from ESP3D's board notes ("USB (Low) / CAN (High)") rather
 * than a Waveshare schematic, and is CONFIRMED on the coach 2026-08-23 --
 * real RV-C traffic reaches the G6 with this value, which it could not do
 * through the wrong mux setting. */
#define BOARD_USB_SEL_CAN_LEVEL true

/* ---- TWAI / CAN (TJA1051) ---------------------------------------------- */
/* NOT the 4.3B's 15/16 — see the header comment. 15/16 are RS485 here. */
#define BOARD_TWAI_TX_GPIO     20
#define BOARD_TWAI_RX_GPIO     19
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
 * Backlight control. Same limitation as the 4.3B: the backlight is gated by
 * CH422G EXIO2, a plain on/off line with no PWM path, so percent maps to
 * on (>0) / off (0) and intermediate dimming is emulated in the UI layer
 * with a translucent overlay (see ui_common).
 */
esp_err_t board_backlight_set_percent(uint8_t percent);

/*
 * Install + start the TWAI driver at 250 kbps, normal mode, accept-all
 * filter (DGN filtering is done in software so sniffer mode sees the whole
 * bus). Also muxes CH422G EXIO5 to CAN, which disables the native USB port.
 * Does not create any tasks.
 */
esp_err_t board_twai_init(void);

#ifdef __cplusplus
}
#endif
