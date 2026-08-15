/*
 * ui_theme — dark night-friendly theme for the RV wall panels.
 *
 * Buttons show label text only (no icon glyph) and swap background color
 * dark blue -> light blue between off/on, driven by ui_dimmer_button.c.
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Near-black background; buttons are dark blue, going light blue when on. */
#define UI_COLOR_BG          lv_color_hex(0x0B0A08)
#define UI_COLOR_STATUSBAR   lv_color_hex(0x121110)
#define UI_COLOR_CARD        lv_color_hex(0x0D1B3A)  /* dark blue, off */
#define UI_COLOR_CARD_ON     lv_color_hex(0x5DADE2)  /* light blue, on */
#define UI_COLOR_CARD_PR     lv_color_hex(0x1B2F55)  /* pressed */
#define UI_COLOR_AMBER       lv_color_hex(0xFFB454)  /* level-bar accent */
#define UI_COLOR_TEXT        lv_color_hex(0xEDE4D3)  /* warm white, on dark bg */
#define UI_COLOR_TEXT_DIM    lv_color_hex(0x8A8375)
#define UI_COLOR_TEXT_ON_LIT lv_color_hex(0x08152B)  /* dark navy, on light bg */
#define UI_COLOR_OFF         lv_color_hex(0x5E594F)
#define UI_COLOR_OK          lv_color_hex(0x3FC46A)  /* CAN healthy */
#define UI_COLOR_ERR         lv_color_hex(0xE5484D)  /* CAN dead */

/* Shared styles, valid after ui_theme_init(). */
extern lv_style_t ui_style_screen;
extern lv_style_t ui_style_statusbar;
extern lv_style_t ui_style_card;
extern lv_style_t ui_style_card_pressed;

void ui_theme_init(void);

#ifdef __cplusplus
}
#endif
