/*
 * Panel: mid_coach (on-screen name "MID COACH") — replaces Entegra SW2-E8
 * (p/n 0291135 / 75570).
 * Build: idf.py -DPANEL=mid_coach build
 *
 * Screen 1 — 2-column x 5-row grid, reading order (row-major):
 *   CENTER CEILING     | MIDSHIP
 *   SIDE CEILING       | SINK/COUNTER
 *   SOFA SCONCE/SLIDE  | DINETTE
 *   ACCENT             | TANK LEVELS
 *   BATTERY            | SHORE POWER
 *
 * The bottom row (issue #57) is what shrank the buttons: build_button_grid()
 * derives its row count from the button count, so going 8 -> 10 entries
 * re-sizes the grid on its own. The seven light buttons and TANK LEVELS kept
 * their previous cells.
 *
 * ⚠️ With more than two screens a nav button MUST name its target screen in
 * instances[0] — 0 = this grid, 1 = tanks, 2 = battery, 3 = shore. The old
 * TANK LEVELS entry used instance_count = 0, which means "toggle between
 * screen 0 and 1"; that only works when those are the only two screens.
 *
 * Screen 2 — tank levels (SeeLevel II 709-RVC, GitHub issue #5), reworked
 * to an animated wave-gauge row + small pinned BACK button (issues #9-#11):
 * FRESH/GREY/BLACK lay out as a centered row of gauges (main/ui/ui.c's
 * build_screen2_row()), BACK as a small button pinned bottom-center. No
 * manual grid/spacer positioning needed for this layout, unlike screen 1.
 * The status bar also reads the GREY/BLACK buttons here by label to drive
 * the header's "Grey-Black OK/Warn/FULL" readout (issue #9).
 *
 * Screens 3 and 4 — battery bank (with the solar readout stacked beneath it)
 * and shore power (issue #57). All three are fed ONLY by ESP-NOW broadcasts
 * from the basement proxy, which holds those BLE links; this panel has no
 * BLE of its own. Being the bridge does not grant that data — main.c had to
 * register the telemetry callback for the bridge role as part of this change.
 *
 * Instance numbers (0=fresh, 1=black, 2=gray) per docs/instance_map.yaml ->
 * tank_sensors -- still unverified against this coach's actual bus traffic,
 * confirm via sniffer (notably the black/grey ordering) before trusting.
 */
#pragma once

#include "lvgl.h"
#include "panel_def.h"

#define PANEL_NAME  "MID COACH"
#define PANEL_INDEX 0

/* This panel is the ESP-NOW bridge/router between the RV-C CAN bus and all
 * remote panels (any panel whose ID ends in _remote, e.g.
 * panels/bedroom_remote.h). It relays remote commands onto this bus and
 * mirrors real status back to them. See main/panel_config.h and
 * components/espnow_link. */
#define PANEL_IS_BRIDGE 1

#define PANEL_HAS_SCREEN_2 1
#define PANEL_HAS_SCREEN_3 1
#define PANEL_HAS_SCREEN_4 1

static const panel_btn_def_t PANEL_BUTTONS[] = {
    { .label = "CENTER CEILING", .type = PANEL_BTN_DIMMER, .instances = {25}, .instance_count = 1 },
    { .label = "MIDSHIP", .type = PANEL_BTN_DIMMER, .instances = {35}, .instance_count = 1 },
    { .label = "SIDE CEILING", .type = PANEL_BTN_DIMMER, .instances = {30, 31}, .instance_count = 2 },
    { .label = "SINK/COUNTER", .type = PANEL_BTN_DIMMER, .instances = {34}, .instance_count = 1 },
    { .label = "SOFA SCONCE/SLIDE", .type = PANEL_BTN_DIMMER, .instances = {32}, .instance_count = 1 },
    { .label = "DINETTE", .type = PANEL_BTN_DIMMER, .instances = {33}, .instance_count = 1 },
    { .label = "ACCENT", .type = PANEL_BTN_DIMMER, .instances = {26, 27}, .instance_count = 2 },
    { .label = "TANK LEVELS", .type = PANEL_BTN_SCREEN_SWITCH, .instances = {1}, .instance_count = 1 },
    { .label = "BATTERY", .type = PANEL_BTN_SCREEN_SWITCH, .instances = {2}, .instance_count = 1 },
    { .label = "SHORE POWER", .type = PANEL_BTN_SCREEN_SWITCH, .instances = {3}, .instance_count = 1 },
};

#define PANEL_BUTTON_COUNT (sizeof(PANEL_BUTTONS) / sizeof(PANEL_BUTTONS[0]))

/*
 * The three dump-valve controls mirror main_cabinet's TANKS section.
 *
 * ⚠️ They are PANEL_BTN_LOCAL_TOGGLE: they flip their own caption and send
 * NOTHING. The valves' actuation is not built yet — the control surface
 * exists so the layout is settled when it is. State is in memory only and
 * does not survive a reboot, and it reflects what someone last tapped, not
 * what the valve is actually doing. They are coloured like any other
 * on-state rather than with the warn colour on purpose: an alarm colour on
 * a button that actuates nothing would announce an open dump valve that
 * does not exist.
 */
static const panel_btn_def_t PANEL_BUTTONS_2[] = {
    { .label = "FRESH", .type = PANEL_BTN_TANK_LEVEL, .instances = {0}, .instance_count = 1 },
    { .label = "GREY", .type = PANEL_BTN_TANK_LEVEL, .instances = {2}, .instance_count = 1 },
    { .label = "BLACK", .type = PANEL_BTN_TANK_LEVEL, .instances = {1}, .instance_count = 1 },
    { .label = "GREY CLOSED", .type = PANEL_BTN_LOCAL_TOGGLE, .instances = {0}, .instance_count = 0, .label_alt = "GREY OPEN" },
    { .label = "BLACK CLOSED", .type = PANEL_BTN_LOCAL_TOGGLE, .instances = {0}, .instance_count = 0, .label_alt = "BLACK OPEN" },
    { .label = "GRAVITY", .type = PANEL_BTN_LOCAL_TOGGLE, .instances = {0}, .instance_count = 0, .label_alt = "MACERATOR" },
    { .label = "BACK", .type = PANEL_BTN_SCREEN_SWITCH, .instances = {0}, .instance_count = 1 },
};

#define PANEL_BUTTON_COUNT_2 (sizeof(PANEL_BUTTONS_2) / sizeof(PANEL_BUTTONS_2[0]))

/*
 * Screen 3: the battery bank. One combined readout, not three gauges — the
 * three Vatrer 300 Ah packs are wired in PARALLEL and read as one bank, the
 * way the coach's own Vatrer display presents it. Tapping it opens the
 * per-pack detail popup. Takes no instances: it aggregates every configured
 * slot itself.
 *
 * The Renogy MPPT readout sits underneath it, matching bedroom_remote's
 * battery screen: solar is what is PUTTING energy into that bank, so the two
 * belong together — 74 W coming in above a bank that says "Fully Charged In
 * 3h" is the whole story in one glance. build_screen2_row() stacks them
 * rather than sharing a row (two full-width readouts can't fit across 480 px)
 * and gives the bank whatever height the fixed-height solar strip leaves.
 */
static const panel_btn_def_t PANEL_BUTTONS_3[] = {
    { .label = "BANK", .type = PANEL_BTN_BATTERY_SUMMARY, .instances = {0}, .instance_count = 0 },
    { .label = "SOLAR", .type = PANEL_BTN_SOLAR, .instances = {0}, .instance_count = 0 },
    { .label = "BACK", .type = PANEL_BTN_SCREEN_SWITCH, .instances = {0}, .instance_count = 1 },
};

#define PANEL_BUTTON_COUNT_3 (sizeof(PANEL_BUTTONS_3) / sizeof(PANEL_BUTTONS_3[0]))

/*
 * Screen 4: shore power (Hughes Power Watchdog) — Line 1 / Line 2 volts,
 * amps, frequency and watts. A 30 A pedestal reports one line and the Line 2
 * column is hidden rather than shown as zeroes.
 */
static const panel_btn_def_t PANEL_BUTTONS_4[] = {
    { .label = "SHORE", .type = PANEL_BTN_SHORE_POWER, .instances = {0}, .instance_count = 0 },
    { .label = "BACK", .type = PANEL_BTN_SCREEN_SWITCH, .instances = {0}, .instance_count = 1 },
};

#define PANEL_BUTTON_COUNT_4 (sizeof(PANEL_BUTTONS_4) / sizeof(PANEL_BUTTONS_4[0]))
