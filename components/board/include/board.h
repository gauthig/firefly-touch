/*
 * board.h — the neutral board-layer header every consumer includes.
 *
 * The project supports more than one display board. Each has its own pin/
 * timing header here, and both implement the same small API:
 * board_display_init(), board_get_display(), board_get_touch_indev(),
 * board_backlight_set_percent(), board_twai_init(). main/ and sim/ include
 * this file and never name a specific board.
 *
 * Which one is compiled is decided by BOARD in the root CMakeLists.txt,
 * derived from PANEL so it cannot be mismatched on the command line — the
 * boards put CAN on different pins, and getting that wrong yields a panel
 * that boots normally and never sees the bus.
 */
#pragma once

#if defined(BOARD_LCD7)
#include "board_lcd7.h"      /* Waveshare ESP32-S3-Touch-LCD-7, landscape */
#else
#include "board_4_3b.h"      /* Waveshare ESP32-S3-Touch-LCD-4.3B, portrait */
#endif
