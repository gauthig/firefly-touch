/*
 * ui_theme — dark night-friendly theme for the RV wall panels.
 *
 * Icons currently use LVGL's built-in symbol font (LV_SYMBOL_*). To move to
 * custom icon fonts later: generate an LVGL font with the icon glyphs
 * (lv_font_conv), add the .c file to this component, and point the
 * panel_btn_def_t .symbol strings at the new private-use codepoints — the
 * widget code only ever treats .symbol as an opaque string.
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Near-black background with warm amber/white accents. */
#define UI_COLOR_BG        lv_color_hex(0x0B0A08)
#define UI_COLOR_STATUSBAR lv_color_hex(0x121110)
#define UI_COLOR_CARD      lv_color_hex(0x1A1815)
#define UI_COLOR_CARD_PR   lv_color_hex(0x2A2620)  /* pressed */
#define UI_COLOR_AMBER     lv_color_hex(0xFFB454)  /* active accent */
#define UI_COLOR_TEXT      lv_color_hex(0xEDE4D3)  /* warm white */
#define UI_COLOR_TEXT_DIM  lv_color_hex(0x8A8375)
#define UI_COLOR_OFF       lv_color_hex(0x5E594F)  /* inactive icon */
#define UI_COLOR_OK        lv_color_hex(0x3FC46A)  /* CAN healthy */
#define UI_COLOR_ERR       lv_color_hex(0xE5484D)  /* CAN dead */

/* Shared styles, valid after ui_theme_init(). */
extern lv_style_t ui_style_screen;
extern lv_style_t ui_style_statusbar;
extern lv_style_t ui_style_card;
extern lv_style_t ui_style_card_pressed;

void ui_theme_init(void);

#ifdef __cplusplus
}
#endif
