/* Simulator stub for board.h — geometry constants + backlight print. */
#pragma once

#include <stdint.h>

#define BOARD_LCD_H_RES 800
#define BOARD_LCD_V_RES 480

int board_backlight_set_percent(uint8_t percent);
