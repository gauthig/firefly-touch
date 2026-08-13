/* Simulator stub for espnow_link.h — only pulled in by ui.c for a
 * PANEL_HAS_CAN=0 build; the fake bus in sim_stubs.c reports the link
 * always healthy and never actually goes over ESP-NOW. */
#pragma once

#include <stdbool.h>

bool espnow_link_healthy(void);
