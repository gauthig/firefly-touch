/*
 * sim_stubs — fake RV-C bus for the PC simulator.
 *
 * twai_enqueue_dimmer_cmd() applies simple G6A-like load behavior to a local
 * instance table and immediately echoes a DC_DIMMER_STATUS_3 back through
 * ui_on_status(), exercising the same status-driven UI path the firmware
 * uses (commands never touch widgets directly — the echo does).
 */
#include <stdio.h>

#include "board_4_3b.h"
#include "rvc_protocol.h"
#include "state_manager.h"
#include "twai_tasks.h"
#include "ui.h"

#define RAMP_STEP 6      /* level units per LONG_PRESSED_REPEAT (~100 ms) */
#define RAMP_FLOOR 2     /* sim keeps ramped-down loads dimly on, not off */

static uint8_t s_level[256];
static bool    s_on[256];
static uint8_t s_memory[256];   /* last non-zero level, for ON restore */

bool state_manager_bus_healthy(void)
{
    return true;
}

int board_backlight_set_percent(uint8_t percent)
{
    printf("[sim] board backlight -> %u%% (on/off only on real hw)\n", percent);
    return 0;
}

bool twai_enqueue_dimmer_cmd(uint8_t instance, rvc_dimmer_cmd_t cmd,
                             uint8_t level, uint8_t duration)
{
    (void)duration;
    uint8_t *lvl = &s_level[instance];
    bool *on = &s_on[instance];
    const uint8_t memory = s_memory[instance] ? s_memory[instance] : RVC_LEVEL_MAX;

    switch (cmd) {
    case RVC_DIMMER_CMD_TOGGLE:
        *on = !*on;
        *lvl = *on ? memory : 0;
        break;
    case RVC_DIMMER_CMD_ON_DELAY:
    case RVC_DIMMER_CMD_ON_DURATION:
        *on = true;
        *lvl = memory;
        break;
    case RVC_DIMMER_CMD_OFF:
    case RVC_DIMMER_CMD_MEMORY_OFF:
        *on = false;
        *lvl = 0;
        break;
    case RVC_DIMMER_CMD_SET_LEVEL:
        *lvl = level > RVC_LEVEL_MAX ? RVC_LEVEL_MAX : level;
        *on = *lvl > 0;
        break;
    case RVC_DIMMER_CMD_RAMP_UP: {
        *on = true;
        const int next = *lvl + RAMP_STEP;
        *lvl = (uint8_t)(next > (int)RVC_LEVEL_MAX ? (int)RVC_LEVEL_MAX : next);
        break;
    }
    case RVC_DIMMER_CMD_RAMP_DOWN:
        *lvl = (uint8_t)(*lvl < RAMP_STEP + RAMP_FLOOR ? RAMP_FLOOR
                                                       : *lvl - RAMP_STEP);
        break;
    case RVC_DIMMER_CMD_STOP:
    default:
        break;
    }

    if (*on && *lvl > 0) {
        s_memory[instance] = *lvl;
    }

    printf("[bus] CMD inst=%u cmd=%u -> STATUS level=%u on=%d\n",
           instance, cmd, *lvl, *on);
    ui_on_status(instance, *lvl, *on);
    return true;
}

/* Called from main_sim to make the screen look alive at startup. */
void sim_seed_demo_state(void)
{
    const struct { uint8_t inst, lvl; } seed[] = {
        { 25, 150 }, { 33, 200 }, { 30, 120 }, { 31, 120 },
    };
    for (unsigned i = 0; i < sizeof(seed) / sizeof(seed[0]); i++) {
        s_level[seed[i].inst] = seed[i].lvl;
        s_on[seed[i].inst] = true;
        s_memory[seed[i].inst] = seed[i].lvl;
        ui_on_status(seed[i].inst, seed[i].lvl, true);
    }
}
