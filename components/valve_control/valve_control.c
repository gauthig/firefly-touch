#include "valve_control.h"

bool valve_mask_is_safe(uint8_t mask)
{
    if ((mask & (VALVE_RELAY_GY_W_HI | VALVE_RELAY_GY_W_LO)) ==
        (VALVE_RELAY_GY_W_HI | VALVE_RELAY_GY_W_LO)) {
        return false;
    }
    if ((mask & (VALVE_RELAY_GY_R_HI | VALVE_RELAY_GY_R_LO)) ==
        (VALVE_RELAY_GY_R_HI | VALVE_RELAY_GY_R_LO)) {
        return false;
    }
    if ((mask & (VALVE_RELAY_BK_W_HI | VALVE_RELAY_BK_W_LO)) ==
        (VALVE_RELAY_BK_W_HI | VALVE_RELAY_BK_W_LO)) {
        return false;
    }
    if ((mask & (VALVE_RELAY_BK_R_HI | VALVE_RELAY_BK_R_LO)) ==
        (VALVE_RELAY_BK_R_HI | VALVE_RELAY_BK_R_LO)) {
        return false;
    }
    return true;
}

uint8_t valve_drive_mask(valve_id_t valve, valve_action_t action)
{
    const bool open = (action == VALVE_ACTION_OPEN);

    if (valve == VALVE_GREY) {
        /* OPEN: WHITE +12V, RED gnd. CLOSE: WHITE gnd, RED +12V. */
        return open ? (VALVE_RELAY_GY_W_HI | VALVE_RELAY_GY_R_LO)
                     : (VALVE_RELAY_GY_W_LO | VALVE_RELAY_GY_R_HI);
    }
    return open ? (VALVE_RELAY_BK_W_HI | VALVE_RELAY_BK_R_LO)
                 : (VALVE_RELAY_BK_W_LO | VALVE_RELAY_BK_R_HI);
}
