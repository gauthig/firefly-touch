/*
 * sim_stubs — fake RV-C bus for the PC simulator.
 *
 * twai_enqueue_dimmer_cmd() applies simple G6A-like load behavior to a local
 * instance table and immediately echoes a DC_DIMMER_STATUS_3 back through
 * ui_on_status(), exercising the same status-driven UI path the firmware
 * uses (commands never touch widgets directly — the echo does).
 */
#include <stdio.h>
#include <stdlib.h>

#include "board_4_3b.h"
#include "bridge_tx.h"
#include "lvgl.h"
#include "rvc_protocol.h"
#include "state_manager.h"
#include "twai_tasks.h"
#include "ui.h"

#define RAMP_STEP 6      /* level units per LONG_PRESSED_REPEAT (~100 ms) */
#define RAMP_FLOOR 2     /* sim keeps ramped-down loads dimly on, not off */

static uint8_t s_level[256];
static bool    s_on[256];
static uint8_t s_memory[256];   /* last non-zero level, for ON restore */

/* Fake tank table: main/state_manager.c isn't linked into the sim (see
 * CMakeLists.txt), so this stub backs state_manager_get_tank() -- ui.c's
 * tank-status header readout polls it directly, same as the real firmware
 * polls the real state manager. */
static uint8_t s_tank_pct[256];
static bool    s_tank_valid[256];

bool state_manager_bus_healthy(void)
{
    return true;
}

bool espnow_link_healthy(void)
{
    return true;
}

bool state_manager_get_tank(uint8_t instance, uint8_t *percent)
{
    if (!s_tank_valid[instance]) {
        return false;
    }
    *percent = s_tank_pct[instance];
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

/* Real firmware picks a backend (CAN vs ESP-NOW) at build time via
 * PANEL_HAS_CAN (main/bridge_tx.c). The simulator has only one fake bus,
 * so both roles drive it directly here — good enough to exercise the UI
 * for a PANEL_HAS_CAN=0 panel, but it never actually goes over ESP-NOW. */
bool bridge_enqueue_dimmer_cmd(uint8_t instance, rvc_dimmer_cmd_t cmd,
                               uint8_t level, uint8_t duration)
{
    return twai_enqueue_dimmer_cmd(instance, cmd, level, duration);
}

/* Sweeps fake TANK_STATUS readings so the wave gauges and the header's
 * Grey-Black OK/Warn/FULL readout (+ backlight-critical override) are all
 * visible and testable in the simulator without touching real hardware.
 * Instance numbers match panels/mid_coach.h: 0=fresh, 1=black, 2=gray. */
static void tank_sweep_timer_cb(lv_timer_t *t)
{
    (void)t;
    /* SIM_TANK_START_TICK lets a one-off --shot capture jump straight to an
     * interesting point in the sweep (e.g. a FULL/blink frame) without
     * waiting for it in real time. Unset in normal interactive use. */
    static uint32_t tick;
    static bool inited;
    if (!inited) {
        inited = true;
        const char *start = getenv("SIM_TANK_START_TICK");
        if (start != NULL) {
            tick = (uint32_t)atoi(start);
        }
    }
    tick++;

    /* Fresh: slow gentle sweep, always valid, never affects the header. */
    uint8_t fresh_pct = (uint8_t)((tick / 2) % 100);
    s_tank_pct[0] = fresh_pct;
    s_tank_valid[0] = true;
    ui_on_tank_status(0, fresh_pct, true);

    /* Grey/black: slower ramp 0->95 and back, so the run cycles through
     * OK (<80) -> Warn (>=80) -> FULL (>=89) -> back down. */
    uint32_t phase = (tick / 4) % 190;
    uint8_t gb_pct = (uint8_t)(phase <= 95 ? phase : 190 - phase);
    s_tank_pct[2] = gb_pct;
    s_tank_valid[2] = true;
    ui_on_tank_status(2, gb_pct, true);
    s_tank_pct[1] = gb_pct;
    s_tank_valid[1] = true;
    ui_on_tank_status(1, gb_pct, true);
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

    lv_timer_create(tank_sweep_timer_cb, 500, NULL);
}
