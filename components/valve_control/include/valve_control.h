/*
 * valve_control — pure C dump-valve relay logic (interlock + drive masks).
 *
 * ESP-free, no ESP-IDF dependencies: every function here is host-testable
 * (see host_test/test_valve_control.c), same split as components/rvc_protocol
 * and components/jbd_bms's *_protocol.c files. The ESP-specific TCA9554 I2C
 * driver, DI sense reads, and the independent watchdog timer live in
 * valve_control_driver.c/.h instead.
 *
 * Relay map and rules from docs/DRAINMASTER-VALVES.md §7. Four relays per
 * valve, wired as an H-bridge: driving both the "to +12V" and "to ground"
 * relay for the same motor wire shorts the house battery across that wire,
 * so valve_mask_is_safe() is the one thing standing in for the two-DPDT
 * design's built-in short-proofing.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Relay bit assignment: bit n = relay CH(n+1). Grey = CH1..CH4 (bits 0-3),
 * black = CH5..CH8 (bits 4-7). "HI" ties the motor wire to +12V, "LO" ties
 * it to ground — see the drive table in DRAINMASTER-VALVES.md §3. */
#define VALVE_RELAY_GY_W_HI  0x01u  /* CH1: grey WHITE -> +12V (opens) */
#define VALVE_RELAY_GY_W_LO  0x02u  /* CH2: grey WHITE -> gnd */
#define VALVE_RELAY_GY_R_HI  0x04u  /* CH3: grey RED   -> +12V */
#define VALVE_RELAY_GY_R_LO  0x08u  /* CH4: grey RED   -> gnd (opens with CH1) */
#define VALVE_RELAY_BK_W_HI  0x10u  /* CH5: black WHITE -> +12V */
#define VALVE_RELAY_BK_W_LO  0x20u  /* CH6: black WHITE -> gnd */
#define VALVE_RELAY_BK_R_HI  0x40u  /* CH7: black RED   -> +12V */
#define VALVE_RELAY_BK_R_LO  0x80u  /* CH8: black RED   -> gnd */

/* Rest state: every relay released, both motor wires floating. */
#define VALVE_MASK_ALL_OFF  0x00u

typedef enum {
    VALVE_GREY = 0,
    VALVE_BLACK,
    VALVE_COUNT,
} valve_id_t;

typedef enum {
    VALVE_ACTION_OPEN = 0,
    VALVE_ACTION_CLOSE,
} valve_action_t;

/* Timing constants, milliseconds — DRAINMASTER-VALVES.md §7 "Timing" table.
 * Named here (not just in the ESP driver) so the host test can assert
 * against them directly. */
#define VALVE_DRIVE_NOMINAL_MS       1000u
#define VALVE_DRIVE_CEILING_MS       2000u  /* independent watchdog, Rule 3 */
#define VALVE_BREAK_BEFORE_MAKE_MS   50u    /* Rule 2 */
#define VALVE_SENSE_DEBOUNCE_MS      50u
#define VALVE_REDRIVE_LOCKOUT_MS     3000u

/*
 * Rule 1: refuses a mask that ties either motor wire of either valve to
 * both rails at once (a dead short across the house battery). Every relay
 * write goes through this — see valve_control_apply() in
 * valve_control_driver.h, the one choke point.
 */
bool valve_mask_is_safe(uint8_t mask);

/*
 * The drive mask for one valve driving OPEN or CLOSE, per the drive table
 * in DRAINMASTER-VALVES.md §3 — always safe by construction (never sets
 * both HI and LO for the same wire), but callers still go through
 * valve_mask_is_safe() at the one choke point rather than trusting that.
 *
 * Only one valve drives at a time in this design, so the other valve's
 * four bits are always 0 in the returned mask.
 */
uint8_t valve_drive_mask(valve_id_t valve, valve_action_t action);

#ifdef __cplusplus
}
#endif
