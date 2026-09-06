/*
 * ui_metrics — pixel sizes and fonts for the shared UI widgets, in one
 * place, with a variant for a large display.
 *
 * Why this exists: main/ui/ui.c and the components/ui_common widgets were
 * full of literal pixel/font values tuned for the 4.3B panels (480x800) and
 * the original 800x480 main_cabinet. The Waveshare 7B is 1024x600 and those
 * values leave it sparse with small-looking text. Rather than scatter
 * `if (width >= 1024)` checks, every such value is named here once and
 * selected at build time.
 *
 * THE GATE: UI_METRICS_LARGE is a compile definition injected by CMake --
 *   - firmware: components/ui_common/CMakeLists.txt sets it PUBLIC when
 *     BOARD == "lcd7b" (which the root CMakeLists.txt maps only from
 *     PANEL == main_cabinet), so it also reaches main/ (ui.c) via REQUIRES.
 *   - simulator: sim/CMakeLists.txt sets it when PANEL == "main_cabinet".
 * No 4.3B panel (BOARD=4_3b) or legacy non-B build (BOARD=lcd7) can satisfy
 * it, so their generated code is byte-identical to before this file
 * existed. That is deliberate: three of the four panels are flashed and
 * installed.
 *
 * The #else branch below reproduces the previous literals EXACTLY. When
 * touching it, diff the value against the git history of the call site --
 * it must not change.
 */
#pragma once

#include "lvgl.h"

#if defined(UI_METRICS_LARGE)

/* ===================================================================== *
 *  Waveshare ESP32-S3-Touch-LCD-7B — 1024x600 landscape (main_cabinet)   *
 *  Rail 210 + statusbar 44  =>  content pane 814 x 556.                   *
 * ===================================================================== */

/* --- fonts --- */
#define UI_FONT_STATUSBAR         (&lv_font_montserrat_20)
#define UI_FONT_SECTION_TITLE     (&lv_font_montserrat_24)
#define UI_FONT_BTN               (&lv_font_montserrat_24)
#define UI_FONT_READOUT_CAPTION   (&lv_font_montserrat_16)
#define UI_FONT_READOUT_VALUE     (&lv_font_montserrat_32)
#define UI_FONT_BATT_VALUE        (&lv_font_montserrat_32)
#define UI_FONT_SOC_PCT           (&lv_font_montserrat_32)
#define UI_FONT_TANK_PCT          (&lv_font_montserrat_32)
#define UI_FONT_BATT_SMALL        (&lv_font_montserrat_20)
#define UI_FONT_POPUP_ROW         (&lv_font_montserrat_20)

/* Long grid-button labels ("BEDROOM CEILING") wrap rather than clip at the
 * bigger font; harmless for short ones. */
#define UI_BTN_LABEL_WRAP         1

/* --- chrome (main/ui/ui.c) --- */
#define UI_STATUSBAR_H            44
#define UI_STATUSBAR_PAD_HOR      20
#define UI_NAV_RAIL_W             210
#define UI_NAV_RAIL_PAD           10
#define UI_NAV_RAIL_GAP           12
#define UI_NAV_RAIL_BTN_H         118
#define UI_GRID_PAD               10
#define UI_CONTENT_PAD_COL        12
#define UI_CONTENT_BOTTOM_OFS     (-8)
#define UI_TANK_TILE_W            220
#define UI_TANK_TILE_H            360

/* --- cards (ui_theme.c) --- */
#define UI_CARD_RADIUS            14
#define UI_CARD_PAD              10

/* --- dimmer button (ui_dimmer_button.c) --- */
#define UI_BTN_PAD_ROW           4
#define UI_DIMMER_BAR_W        150
#define UI_DIMMER_BAR_H          7

/* --- tank wave gauge (ui_tank_wave.c) --- */
#define UI_TANK_WAVE_AMP         8
#define UI_TANK_WAVE_STEP        8
#define UI_TANK_WAVE_MAX_PTS    48
#define UI_TANK_WAVE_ANIMATE     0   /* static: full_refresh RGB blinks on per-frame redraws */
#define UI_TANK_GLASS_W        200
#define UI_TANK_GLASS_H        300
#define UI_TANK_GLASS_BORDER     3
#define UI_TANK_GLASS_RADIUS    18
#define UI_TANK_PCT_OFS          8

/* --- battery summary (ui_battery_summary.c) --- */
#define UI_BATT_WRAP_PAD         6
#define UI_BATT_WRAP_ROW         8
#define UI_SOC_ARC_SZ         240
#define UI_SOC_ARC_W            16
#define UI_BATT_GRID_H        230
#define UI_BATT_STRIP_H        40
#define UI_BATT_POPUP_W       560
#define UI_BATT_POPUP_PAD      18
#define UI_BATT_POPUP_ROW     12
#define UI_BATT_POPUP_RADIUS  14

/* --- shore / solar readout tiles (ui_shore_panel.c, ui_solar_panel.c) --- */
#define UI_TILE_RADIUS         12
#define UI_TILE_PAD             6
#define UI_SHORE_ROOT_PAD       6
#define UI_SHORE_COL_GAP       12
#define UI_SHORE_COL_ROW       10
#define UI_SOLAR_ROOT_PAD       6
#define UI_SOLAR_ROOT_ROW       8
#define UI_SOLAR_HEADER_H      40
#define UI_SOLAR_GRID_GAP      12

#else /* ------------------- 4.3B portrait + legacy 800x480 7" ------------- */

/* Values below are the literals that were inline in the call sites before
 * ui_metrics.h. Do not change them -- three panels ship with them. */

#define UI_FONT_STATUSBAR         (&lv_font_montserrat_16)
#define UI_FONT_SECTION_TITLE     (&lv_font_montserrat_20)
#define UI_FONT_BTN               (&lv_font_montserrat_20)
#define UI_FONT_READOUT_CAPTION   (&lv_font_montserrat_14)
#define UI_FONT_READOUT_VALUE     (&lv_font_montserrat_28)
#define UI_FONT_BATT_VALUE        (&lv_font_montserrat_24)
#define UI_FONT_SOC_PCT           (&lv_font_montserrat_28)
#define UI_FONT_TANK_PCT          (&lv_font_montserrat_20)
#define UI_FONT_BATT_SMALL        (&lv_font_montserrat_16)
#define UI_FONT_POPUP_ROW         (&lv_font_montserrat_16)

#define UI_BTN_LABEL_WRAP         0

#define UI_STATUSBAR_H           36
#define UI_STATUSBAR_PAD_HOR     14
#define UI_NAV_RAIL_W           168
#define UI_NAV_RAIL_PAD           6
#define UI_NAV_RAIL_GAP           6
#define UI_NAV_RAIL_BTN_H        72
#define UI_GRID_PAD               3
#define UI_CONTENT_PAD_COL        6
#define UI_CONTENT_BOTTOM_OFS   (-6)
#define UI_TANK_TILE_W          140
#define UI_TANK_TILE_H          200

#define UI_CARD_RADIUS          10
#define UI_CARD_PAD              6

#define UI_BTN_PAD_ROW           2
#define UI_DIMMER_BAR_W         90
#define UI_DIMMER_BAR_H          5

#define UI_TANK_WAVE_AMP         4
#define UI_TANK_WAVE_STEP        6
#define UI_TANK_WAVE_MAX_PTS    32
#define UI_TANK_WAVE_ANIMATE     1
#define UI_TANK_GLASS_W         90
#define UI_TANK_GLASS_H         90
#define UI_TANK_GLASS_BORDER     2
#define UI_TANK_GLASS_RADIUS    12
#define UI_TANK_PCT_OFS          4

#define UI_BATT_WRAP_PAD         4
#define UI_BATT_WRAP_ROW         6
#define UI_SOC_ARC_SZ         210
#define UI_SOC_ARC_W           14
#define UI_BATT_GRID_H        200
#define UI_BATT_STRIP_H        30
#define UI_BATT_POPUP_W       LV_PCT(92)
#define UI_BATT_POPUP_PAD     14
#define UI_BATT_POPUP_ROW     10
#define UI_BATT_POPUP_RADIUS  10

#define UI_TILE_RADIUS          8
#define UI_TILE_PAD             4
#define UI_SHORE_ROOT_PAD       4
#define UI_SHORE_COL_GAP        8
#define UI_SHORE_COL_ROW        6
#define UI_SOLAR_ROOT_PAD       4
#define UI_SOLAR_ROOT_ROW       4
#define UI_SOLAR_HEADER_H      26
#define UI_SOLAR_GRID_GAP       6

#endif /* UI_METRICS_LARGE */
