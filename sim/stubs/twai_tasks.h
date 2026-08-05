/* Simulator stub for twai_tasks.h — commands go to the fake bus in
 * sim_stubs.c, which echoes DC_DIMMER_STATUS_3 back into ui_on_status(). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "rvc_protocol.h"

bool twai_enqueue_dimmer_cmd(uint8_t instance, rvc_dimmer_cmd_t cmd,
                             uint8_t level, uint8_t duration);
