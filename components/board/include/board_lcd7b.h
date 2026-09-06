/*
 * board_lcd7b — bring-up for the Waveshare ESP32-S3-Touch-LCD-7B (1024x600).
 *
 * Board: ESP32-S3-WROOM-1 (N16R8), 7" 1024x600 RGB565 IPS LCD, GT911
 * capacitive touch on I2C, Waveshare "IO_EXTENSION" IO expander (NOT a
 * CH422G -- see below), TJA1051 CAN transceiver, RS485 transceiver,
 * 7-36 V supply.
 *
 * Near-copy of board_lcd7.{c,h} (the non-B 800x480 7"), but the 7B differs
 * by MORE than geometry. Verified against an I2C scan of the real board and
 * the bench-proven ESPHome port
 * (github.com/xtux77/waveshare-esp32s3-lcd7b-esphome, docs/hardware.md).
 * It matches the non-B for the RGB data/sync pins, I2C bus, GT911 (INT
 * GPIO4, addr 0x5D), and CAN on GPIO20 (TX) / GPIO19 (RX).
 *
 * Differences from board_lcd7 (and why a separate file):
 *
 *   - 1024x600 instead of 800x480, and a different RGB porch/pulse block +
 *     30 MHz pclk (see board_lcd7b.c -- ESPHome WAVESHARE-5-1024X600
 *     timings, NOT the numbers in the NicoEFI "7B demo" fork, which are
 *     still 800x480 and give a scrambled panel).
 *
 *   - The IO expander is a DIFFERENT CHIP. board_lcd7 uses a CH422G
 *     (`components/ch422g`: register-less, split across I2C 0x24/0x38). The
 *     7B has Waveshare's register-addressed "IO_EXTENSION" at 0x24 only
 *     (`components/ws_io_expander`: 2-byte {reg,val} writes, and a real PWM
 *     output). An I2C scan of the board confirms it: 0x24 and the GT911 at
 *     0x5D answer; the CH422G's 0x23/0x38 do not.
 *
 *   - ⚠️ ONE EXTRA ENABLE PIN. The 7B has EXIO6 = LCD_VDD_EN (VCOM supply)
 *     that the non-B lacks. It must be driven HIGH before the RGB panel
 *     comes up or the screen stays black with the backlight lit -- the
 *     "double enable pin" trap. board_display_init() handles it.
 *
 *   - board_lcd7.{c,h} is flashed and coach-verified on the non-B panel.
 *     Keeping the 7B separate means bring-up churn here can never regress
 *     it -- the same reason board_lcd7 is itself a near-copy of board_4_3b
 *     rather than a shared implementation.
 *
 * Everything else carries over from board_lcd7 unchanged:
 *
 *   - CAN is on GPIO20 (TX) / GPIO19 (RX). On the 4.3B those pins are
 *     15/16, and 15/16 drive the RS485 transceiver here. A 4.3B-configured
 *     binary flashed here talks to RS485 and the CAN bus stays silent with
 *     no error anywhere.
 *
 *   - GPIO19/20 are also the native USB D-/D+ lines. IO_EXTENSION EXIO5 muxes
 *     between them: low = USB, high = CAN. board_twai_init() raises EXIO5,
 *     which means THE NATIVE USB PORT STOPS WORKING once CAN is up -- flash
 *     and monitor this board over its UART (CH343) port.
 *
 *   - Landscape, no rotation. Used in its native orientation so the
 *     side-nav rail has width to live in.
 *
 * ============================ MERGE POINT ============================
 * RGB timings in board_lcd7b.c: ESPHome's WAVESHARE-5-1024X600 porches
 * (same panel), pclk_active_neg = 1, hsync pw/bp/fp = 30/145/170, vsync
 * 2/23/12. pclk is 21 MHz here, NOT the 30 MHz the wiki/xtux77 use --
 * board_lcd7b.c runs full_refresh (whole frame every refresh) to kill
 * flicker, and above ~21 MHz that render can't stay ahead of the scanout
 * and the panel shakes. The buffer-mode / pclk tuning was bench-fought
 * 2026-09-05; see the CLAUDE.md "7B migration" gotcha before changing it.
 * Do not trust the NicoEFI "7B demo" repo -- its porches are still 800x480.
 * =====================================================================
 */
#pragma once

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- LCD geometry --------------------------------------------------- */
#define BOARD_LCD_H_RES   1024
#define BOARD_LCD_V_RES   600

/* ---- RGB LCD pins (identical to the 4.3B and the non-B 7") ---------- */
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

/* ---- I2C (GT911 touch + IO_EXTENSION expander share the bus) --------- */
#define BOARD_I2C_GPIO_SDA     8
#define BOARD_I2C_GPIO_SCL     9
#define BOARD_I2C_FREQ_HZ      400000
#define BOARD_TOUCH_GPIO_INT   4

/* ---- IO_EXTENSION expander pins (EXIOn) ----------------------------- */
#define BOARD_EXIO_TP_RST      1   /* GT911 reset */
#define BOARD_EXIO_DISP        2   /* LCD backlight enable (on/off) */
#define BOARD_EXIO_LCD_RST     3
#define BOARD_EXIO_SD_CS       4
#define BOARD_EXIO_USB_SEL     5   /* low = native USB, high = CAN */
/* ⚠️ EXIO6 = LCD_VDD_EN, the panel's VCOM-supply enable. The non-B 7"
 * does NOT have this pin; the 7B needs it driven HIGH before the RGB panel
 * comes up or the screen stays black (backlight on, no image). This is the
 * "double enable pin" trap of the 7B. */
#define BOARD_EXIO_LCD_VDD_EN  6

/* USB_SEL level that routes GPIO19/20 to the CAN transceiver.
 * Same polarity as the non-B 7" ("USB (Low) / CAN (High)" per ESP3D and
 * the 7B demo's io_extension.h: "IO5 (Select communication interface:
 * 0 for USB, 1 for CAN)"). Coach-verified on the non-B board 2026-08-23;
 * re-verify on the 7B at bring-up. */
#define BOARD_USB_SEL_CAN_LEVEL true

/* ---- TWAI / CAN (TJA1051) ---------------------------------------------- */
/* NOT the 4.3B's 15/16 -- see the header comment. 15/16 are RS485 here.
 * 7B demo lib/twai/twai.h: TX_GPIO_NUM = GPIO_NUM_20, RX_GPIO_NUM = GPIO_NUM_19. */
#define BOARD_TWAI_TX_GPIO     20
#define BOARD_TWAI_RX_GPIO     19
#define BOARD_TWAI_BITRATE_KBPS 250

/* ---- API ---------------------------------------------------------------- */

/*
 * Full display bring-up: I2C bus, IO_EXTENSION (panel out of reset, backlight),
 * RGB panel, GT911 touch, esp_lvgl_port (LVGL task pinned to core 1).
 * Call once from app_main before any UI code.
 */
esp_err_t board_display_init(void);

lv_display_t *board_get_display(void);
lv_indev_t   *board_get_touch_indev(void);

/*
 * Backlight control. percent maps to on (>0) / off (0), driving
 * IO_EXTENSION EXIO2 as a plain output bit -- the same contract as the
 * 4.3B / non-B 7", so the UI's translucent-overlay idle-dim still applies.
 * (Unlike the CH422G, this expander HAS a real PWM output --
 * ws_io_expander_set_backlight_pct() -- so a future change could do the dim
 * in hardware. Kept on/off here for parity.)
 */
esp_err_t board_backlight_set_percent(uint8_t percent);

/*
 * Install + start the TWAI driver at 250 kbps, normal mode, accept-all
 * filter (DGN filtering is done in software so sniffer mode sees the whole
 * bus). Also muxes IO_EXTENSION EXIO5 to CAN, which disables the native USB port.
 * Does not create any tasks.
 */
esp_err_t board_twai_init(void);

#ifdef __cplusplus
}
#endif
