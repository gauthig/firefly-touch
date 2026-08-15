/* Simulator stub for state_manager.h — the fake bus is always healthy. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

bool state_manager_bus_healthy(void);

/* Backed by sim_stubs.c's fake tank table, fed by its sweep timer. */
bool state_manager_get_tank(uint8_t instance, uint8_t *percent);
