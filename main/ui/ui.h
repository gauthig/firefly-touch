#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Builds the panel screen (status bar + button grid). Takes the LVGL lock
 * itself; call after board_display_init(). */
void ui_init(void);

/*
 * Push a status change into the UI. Called by the state manager (core 0);
 * takes the LVGL lock internally. Safe before ui_init (no-op).
 */
void ui_on_status(uint8_t instance, uint8_t level, bool on);
