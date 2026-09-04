/*
 * Host-runnable unit tests for valve_control (plain asserts, no framework).
 *
 * Build & run with any host C compiler, e.g.:
 *   gcc -Wall -Wextra -I../include ../valve_control.c test_valve_control.c -o test_valve_control && ./test_valve_control
 */
#include <assert.h>
#include <stdio.h>

#include "valve_control.h"

static void test_mask_is_safe_rejects_shorts(void)
{
    /* Each forbidden combination from DRAINMASTER-VALVES.md §7 Rule 1. */
    assert(!valve_mask_is_safe(VALVE_RELAY_GY_W_HI | VALVE_RELAY_GY_W_LO));
    assert(!valve_mask_is_safe(VALVE_RELAY_GY_R_HI | VALVE_RELAY_GY_R_LO));
    assert(!valve_mask_is_safe(VALVE_RELAY_BK_W_HI | VALVE_RELAY_BK_W_LO));
    assert(!valve_mask_is_safe(VALVE_RELAY_BK_R_HI | VALVE_RELAY_BK_R_LO));

    /* A short on one valve is still refused even with the other valve's
     * bits set safely alongside it. */
    assert(!valve_mask_is_safe(VALVE_RELAY_GY_W_HI | VALVE_RELAY_GY_W_LO |
                                VALVE_RELAY_BK_W_HI));

    /* All eight relays energized at once is two simultaneous shorts. */
    assert(!valve_mask_is_safe(0xFFu));
}

static void test_mask_is_safe_accepts_valid_states(void)
{
    assert(valve_mask_is_safe(VALVE_MASK_ALL_OFF));

    /* Grey OPEN, black at rest. */
    assert(valve_mask_is_safe(VALVE_RELAY_GY_W_HI | VALVE_RELAY_GY_R_LO));
    /* Grey CLOSE, black at rest. */
    assert(valve_mask_is_safe(VALVE_RELAY_GY_W_LO | VALVE_RELAY_GY_R_HI));
    /* Black OPEN, grey at rest. */
    assert(valve_mask_is_safe(VALVE_RELAY_BK_W_HI | VALVE_RELAY_BK_R_LO));
    /* Black CLOSE, grey at rest. */
    assert(valve_mask_is_safe(VALVE_RELAY_BK_W_LO | VALVE_RELAY_BK_R_HI));

    /* Each individual relay clicked alone (bring-up step 1) is always
     * safe — a lone HI or LO bit can never be a same-wire short. */
    for (uint8_t bit = 0; bit < 8; bit++) {
        assert(valve_mask_is_safe((uint8_t)(1u << bit)));
    }
}

static void test_drive_mask_matches_drive_table(void)
{
    /* DRAINMASTER-VALVES.md §3 drive table, grey rows. */
    uint8_t m = valve_drive_mask(VALVE_GREY, VALVE_ACTION_OPEN);
    assert(m == (VALVE_RELAY_GY_W_HI | VALVE_RELAY_GY_R_LO));
    assert(valve_mask_is_safe(m));
    assert((m & (VALVE_RELAY_BK_W_HI | VALVE_RELAY_BK_W_LO |
                 VALVE_RELAY_BK_R_HI | VALVE_RELAY_BK_R_LO)) == 0);

    m = valve_drive_mask(VALVE_GREY, VALVE_ACTION_CLOSE);
    assert(m == (VALVE_RELAY_GY_W_LO | VALVE_RELAY_GY_R_HI));
    assert(valve_mask_is_safe(m));

    /* Black is the same table on CH5-CH8. */
    m = valve_drive_mask(VALVE_BLACK, VALVE_ACTION_OPEN);
    assert(m == (VALVE_RELAY_BK_W_HI | VALVE_RELAY_BK_R_LO));
    assert(valve_mask_is_safe(m));
    assert((m & (VALVE_RELAY_GY_W_HI | VALVE_RELAY_GY_W_LO |
                 VALVE_RELAY_GY_R_HI | VALVE_RELAY_GY_R_LO)) == 0);

    m = valve_drive_mask(VALVE_BLACK, VALVE_ACTION_CLOSE);
    assert(m == (VALVE_RELAY_BK_W_LO | VALVE_RELAY_BK_R_HI));
    assert(valve_mask_is_safe(m));
}

static void test_timing_constants(void)
{
    /* DRAINMASTER-VALVES.md §7 "Timing" table, pinned so a future edit to
     * either place is caught here. */
    assert(VALVE_DRIVE_NOMINAL_MS == 1000u);
    assert(VALVE_DRIVE_CEILING_MS == 2000u);
    assert(VALVE_BREAK_BEFORE_MAKE_MS == 50u);
    assert(VALVE_SENSE_DEBOUNCE_MS == 50u);
    assert(VALVE_REDRIVE_LOCKOUT_MS == 3000u);
    assert(VALVE_DRIVE_NOMINAL_MS < VALVE_DRIVE_CEILING_MS);
}

int main(void)
{
    test_mask_is_safe_rejects_shorts();
    test_mask_is_safe_accepts_valid_states();
    test_drive_mask_matches_drive_table();
    test_timing_constants();
    printf("all valve_control tests passed\n");
    return 0;
}
