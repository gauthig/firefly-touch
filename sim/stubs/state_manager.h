/* Simulator stub for state_manager.h — the fake bus is always healthy. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

bool state_manager_bus_healthy(void);

/* Backed by sim_stubs.c's fake tank table, fed by its sweep timer. */
bool state_manager_get_tank(uint8_t instance, uint8_t *percent);

/* Same signature as the real header. sim_stubs.c walks its fake bus table,
 * so the light-master button's "is any light on" check and its off-sweep
 * behave in the simulator exactly as they do on a panel. */
typedef void (*state_status_sink_t)(uint8_t instance, uint8_t level, bool on, void *ctx);

void state_manager_for_each_known(state_status_sink_t cb, void *ctx);
