/*
 * rvc_protocol — RV-C DGN encode/decode and 29-bit CAN ID pack/unpack.
 *
 * Pure C, no ESP-IDF dependencies: every function here is host-testable
 * (see host_test/test_rvc.c).
 *
 * RV-C 29-bit identifier layout (per RV-C spec section on message format):
 *   bits 26..28  priority (3 bits, default 6 for normal traffic)
 *   bit  25      reserved (0)
 *   bits 8..24   DGN (17 bits: data page bit + 16-bit PGN-style number)
 *   bits 0..7    source address
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- DGNs used by this project ---------------------------------------- */
#define RVC_DGN_DC_DIMMER_COMMAND_2  0x1FEDBu
#define RVC_DGN_DC_DIMMER_STATUS_3   0x1FEDAu

#define RVC_PRIORITY_DEFAULT  6u

/* Desired/operating level is 0..200 in 0.5 % steps (200 = 100 %). */
#define RVC_LEVEL_MAX   200u
/* 0xFF in a level/duration/group field means "no change / not used". */
#define RVC_FIELD_NA    0xFFu

/*
 * DC_DIMMER_COMMAND_2 command codes (standard RV-C command enum).
 *
 * Values cross-checked 2026-08-08 against two independent transcriptions of
 * the RV-C spec used by proven-working implementations (linuxkidd/rvc-proxy
 * dc_dimmer.pl and carpenike/rvc2hass rvc-spec.yml, both run on real
 * Spyder/Firefly-era coaches). An earlier revision of this enum had
 * 17=RAMP_UP / 18=RAMP_DOWN — WRONG: 17 is "ramp brightness" and 18 is
 * "ramp toggle". Sending 17 repeatedly during hold-to-dim opened a ramp
 * session the G6A never exited, after which that load ignored ALL panels
 * (including factory ones) until the G6 was power-cycled. Do not renumber
 * these without re-checking against a captured factory frame or the spec.
 */
typedef enum {
    RVC_DIMMER_CMD_SET_LEVEL          = 0,   /* set brightness (with optional delay) */
    RVC_DIMMER_CMD_ON_DURATION        = 1,   /* on for <duration> */
    RVC_DIMMER_CMD_ON_DELAY           = 2,   /* on (after optional delay) */
    RVC_DIMMER_CMD_OFF                = 3,
    RVC_DIMMER_CMD_STOP               = 4,   /* stop an in-progress ramp */
    RVC_DIMMER_CMD_TOGGLE             = 5,
    RVC_DIMMER_CMD_MEMORY_OFF         = 6,
    RVC_DIMMER_CMD_RAMP_BRIGHTNESS    = 17,  /* ramp to <level> */
    RVC_DIMMER_CMD_RAMP_TOGGLE        = 18,  /* ramp, alternating direction */
    RVC_DIMMER_CMD_RAMP_UP            = 19,
    RVC_DIMMER_CMD_RAMP_DOWN          = 20,
    RVC_DIMMER_CMD_RAMP_UP_DOWN       = 21,
    RVC_DIMMER_CMD_LOCK               = 33,
    RVC_DIMMER_CMD_UNLOCK             = 34,
    RVC_DIMMER_CMD_FLASH              = 49,
    RVC_DIMMER_CMD_FLASH_MOMENTARILY  = 50,
} rvc_dimmer_cmd_t;

/* ---- 29-bit ID pack/unpack -------------------------------------------- */

typedef struct {
    uint8_t  priority;      /* 0..7 */
    uint32_t dgn;           /* 17-bit DGN */
    uint8_t  source_addr;
} rvc_id_t;

uint32_t rvc_id_pack(uint8_t priority, uint32_t dgn, uint8_t source_addr);
rvc_id_t rvc_id_unpack(uint32_t can_id);

/* ---- DC_DIMMER_COMMAND_2 (DGN 0x1FEDB), transmit ---------------------- */

typedef struct {
    uint8_t          instance;
    uint8_t          group;      /* group bitmask; RVC_FIELD_NA = no group addressing */
    uint8_t          level;      /* 0..200 = 0..100 % in 0.5 % steps; RVC_FIELD_NA = no change */
    rvc_dimmer_cmd_t command;
    uint8_t          duration;   /* seconds; RVC_FIELD_NA = none */
} rvc_dimmer_command_t;

/* Fills data[0..7] with the encoded payload (always 8 bytes on the wire). */
void rvc_encode_dc_dimmer_command_2(const rvc_dimmer_command_t *cmd, uint8_t data[8]);

/* ---- DC_DIMMER_STATUS_3 (DGN 0x1FEDA), receive ------------------------ */

typedef struct {
    uint8_t instance;
    uint8_t group;
    uint8_t operating_level;     /* 0..200 */
    uint8_t lock_status;         /* raw byte 3 (lock bits) */
    uint8_t duration_remaining;  /* raw byte 4 */
    uint8_t last_command;        /* raw byte 5 */
    uint8_t status_bits;         /* raw byte 6: overcurrent/override/enable bits */
    bool    load_on;             /* derived: operating_level > 0 (see TODO in .c) */
} rvc_dimmer_status_t;

/* Returns false if the frame is too short to be a valid STATUS_3. */
bool rvc_decode_dc_dimmer_status_3(const uint8_t *data, size_t len, rvc_dimmer_status_t *out);

/* ---- Level helpers ----------------------------------------------------- */

static inline uint8_t rvc_level_to_percent(uint8_t level)
{
    return (uint8_t)((level > RVC_LEVEL_MAX ? RVC_LEVEL_MAX : level) / 2);
}

static inline uint8_t rvc_percent_to_level(uint8_t percent)
{
    return (uint8_t)(percent >= 100 ? RVC_LEVEL_MAX : percent * 2);
}

#ifdef __cplusplus
}
#endif
