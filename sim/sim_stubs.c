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

#include "board.h"
#include "bridge_tx.h"
#include "lvgl.h"
#include "renogy_solar_protocol.h"   /* RENOGY_CHARGE_* for the fake sweep */
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

/* Backs the light-master button: ui.c walks every instance with known state
 * to answer "is any light on" and to sweep them off. The fake bus table
 * above IS that state here, so master behaves in the sim exactly as it does
 * on a real panel -- including reaching instances that have no button. */
void state_manager_for_each_known(state_status_sink_t cb, void *ctx)
{
    for (unsigned i = 0; i < 256; i++) {
        if (s_level[i] != 0 || s_on[i]) {
            cb((uint8_t)i, s_level[i], s_on[i], ctx);
        }
    }
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

/*
 * Group-addressed command, as the LIGHT MASTER button sends. The real G6
 * expands one of these into a status frame per load in the group; there are
 * no real groups here, so the fake bus applies it to every instance it has
 * ever seen — which is what makes the master button's own state readout
 * behave in the simulator.
 *
 * MEMORY_OFF / RVC_LEVEL_RESTORE are honoured rather than flattened to
 * off/100 %, because remembering the level is the whole point of the pair
 * and a sim that ignored it would hide a regression in exactly the thing
 * this feature exists for.
 */
bool bridge_enqueue_dimmer_group_cmd(uint8_t group, rvc_dimmer_cmd_t cmd,
                                     uint8_t level)
{
    printf("[sim] group 0x%02X cmd=%u level=%u\n", group, cmd, level);
    for (unsigned inst = 0; inst < 256; inst++) {
        /* "Known" has to include a load that MEMORY_OFF just zeroed, or the
         * restore half of the cycle would never find it again — note this is
         * deliberately a looser test than the one for_each_known() uses. */
        if (s_level[inst] == 0 && !s_on[inst] && s_memory[inst] == 0) {
            continue;
        }
        if (cmd == RVC_DIMMER_CMD_MEMORY_OFF) {
            if (s_on[inst]) {
                s_memory[inst] = s_level[inst];   /* remember for the restore */
            }
            s_level[inst] = 0;
            s_on[inst] = false;
        } else if (level == RVC_LEVEL_RESTORE) {
            if (s_memory[inst] > 0) {
                s_level[inst] = s_memory[inst];
                s_on[inst] = true;
            }
        } else {
            s_level[inst] = level;
            s_on[inst] = level > 0;
        }
        ui_on_status((uint8_t)inst, s_level[inst], s_on[inst]);
    }
    return true;
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

/* Sweeps 3 synthetic battery readings so the combined bank readout, its SOC
 * color bands, the direction-aware ETA caption, the F temperature high/low
 * and the "N of M" pack indicator are all exercisable without real BLE.
 *
 * These go in through ui_on_battery_status(), exactly like the ESP-NOW
 * telemetry broadcasts the basement proxy sends on real hardware -- no
 * panel talks to a BMS itself any more, so there is no client left to fake.
 * jbd_bms_combine() then runs for real inside ui.c, which is the point: the
 * sim exercises the actual aggregation, not a stand-in for it.
 *
 * The three packs deliberately differ: pack 0 charges, pack 1 discharges on
 * the opposite phase, and pack 2 drops OFFLINE for part of the cycle --
 * that last one is what exercises the shrinking-bank path and the amber
 * "2 of 3" indicator, which is otherwise only reachable by physically
 * powering down a battery on the bench.
 */
static void battery_sweep_timer_cb(lv_timer_t *t)
{
    (void)t;
    /* SIM_BATTERY_START_TICK jumps the sweep to a chosen point, same trick
     * (and same reason) as SIM_TANK_START_TICK above: a --shot capture only
     * runs ~2 s of real time, which is far too short to reach the offline
     * window naturally. Unset in normal interactive use. */
    static uint32_t tick;
    static bool inited;
    if (!inited) {
        inited = true;
        const char *start = getenv("SIM_BATTERY_START_TICK");
        if (start != NULL) {
            tick = (uint32_t)atoi(start);
        }
    }
    tick++;

    const uint32_t phase0 = (tick / 3) % 200;
    const uint8_t pct0 = (uint8_t)(phase0 <= 100 ? phase0 : 200 - phase0);
    ui_battery_pack_t p0 = {
        .slot             = 0,
        .online           = true,
        .soc_percent      = pct0,
        .voltage_v        = 13.2f,
        .current_a        = pct0 < 100 ? 15.0f : 0.0f,
        .residual_ah      = 300.0f * (float)pct0 / 100.0f,
        .full_capacity_ah = 300.0f,
        .temp_valid       = true,
        .temp_min_c       = 24.5f,
        .temp_max_c       = 25.5f,
    };
    ui_on_battery_status(&p0);

    const uint32_t phase1 = (tick / 3 + 100) % 200;
    const uint8_t pct1 = (uint8_t)(phase1 <= 100 ? phase1 : 200 - phase1);
    ui_battery_pack_t p1 = {
        .slot             = 1,
        .online           = true,
        .soc_percent      = pct1,
        .voltage_v        = 12.6f,
        .current_a        = pct1 > 0 ? -8.0f : 0.0f,
        .residual_ah      = 300.0f * (float)pct1 / 100.0f,
        .full_capacity_ah = 300.0f,
        .temp_valid       = true,
        .temp_min_c       = 21.0f,
        .temp_max_c       = 22.0f,
    };
    ui_on_battery_status(&p1);

    /* Offline for roughly a quarter of the sweep. The proxy keeps
     * broadcasting a pack it has lost the link to, flagged offline, which
     * is exactly what this reproduces -- the panel must show "2 of 3"
     * rather than simply going quiet. */
    ui_battery_pack_t p2 = {
        .slot             = 2,
        .online           = ((tick / 20) % 4) != 3,
        .soc_percent      = 15,
        .voltage_v        = 12.1f,
        .current_a        = 0.0f,
        .residual_ah      = 45.0f,
        .full_capacity_ah = 300.0f,
        .temp_valid       = true,
        .temp_min_c       = 30.0f,
        .temp_max_c       = 31.5f,
    };
    ui_on_battery_status(&p2);
}

/* Fakes the shore-power telemetry that the basement BLE proxy would
 * broadcast, so the Line 1 / Line 2 screen is reviewable without a Power
 * Watchdog (or the proxy) present. Values drift slightly so a capture shows
 * plausible live numbers rather than suspiciously round ones. */
static void shore_power_timer_cb(lv_timer_t *t)
{
    (void)t;
    static uint32_t tick;
    tick++;

    const float wobble = (float)(tick % 7) * 0.5f;
    ui_shore_power_t sp = {
        .line_count   = 2,
        .error_code   = 0,
        .frequency_hz = 60.0f,
        .volts = { 118.0f + wobble * 0.2f, 119.0f - wobble * 0.2f },
        .amps  = { 35.0f - wobble,         27.0f + wobble },
    };
    sp.watts[0] = sp.volts[0] * sp.amps[0];
    sp.watts[1] = sp.volts[1] * sp.amps[1];
    ui_on_shore_power(&sp);
}

/*
 * Fakes the Renogy MPPT telemetry the basement proxy broadcasts, so the
 * solar readout is reviewable without a controller (or the proxy) present.
 *
 * Sweeps through a plausible day rather than sitting on one value: PV output
 * rises and falls, and the charging state follows it (boost while there is
 * real current, float once it tails off, off in the dark). That is what
 * exercises the state colouring and the "0 W because it is dark" vs "0 W
 * because it is full" distinction the widget exists to show.
 *
 * SIM_SOLAR_START_TICK=<n> jumps the sweep to a chosen point -- same trick
 * and same reason as SIM_TANK_START_TICK, since a --shot capture only runs
 * about 2 s of real time and would never reach the dark end naturally.
 */
static void solar_sweep_timer_cb(lv_timer_t *t)
{
    (void)t;
    static uint32_t tick = UINT32_MAX;
    if (tick == UINT32_MAX) {
        const char *env = getenv("SIM_SOLAR_START_TICK");
        tick = (env != NULL) ? (uint32_t)atoi(env) : 0;
    }
    tick++;

    /* 0..100..0 over the cycle: a day's arc, compressed. */
    const uint32_t phase = tick % 120u;
    const float frac = (phase < 60u) ? (float)phase / 60.0f
                                     : (float)(120u - phase) / 60.0f;

    ui_solar_status_t sol = {
        .online            = true,
        .battery_soc       = 100,
        .battery_volts     = 13.2f + frac * 1.5f,
        .pv_volts          = frac > 0.02f ? 17.0f + frac * 4.0f : 0.0f,
        .pv_amps           = frac * 5.2f,
        .temp_valid        = true,
        .controller_temp_f = 95.0f + frac * 30.0f,
        .battery_temp_f    = 88.0f + frac * 20.0f,
    };
    sol.pv_watts = sol.pv_volts * sol.pv_amps;

    if (sol.pv_amps > 2.0f) {
        sol.charge_state = RENOGY_CHARGE_BOOST;
    } else if (sol.pv_amps > 0.1f) {
        sol.charge_state = RENOGY_CHARGE_FLOATING;
    } else {
        sol.charge_state = RENOGY_CHARGE_DEACTIVATED;
    }

    ui_on_solar_status(&sol);
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
    lv_timer_create(battery_sweep_timer_cb, 500, NULL);
    lv_timer_create(shore_power_timer_cb, 500, NULL);
    lv_timer_create(solar_sweep_timer_cb, 500, NULL);
}
