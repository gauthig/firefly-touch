/*
 * easytouch_protocol — Micro-Air EasyTouch RV thermostat JSON-over-BLE codec.
 *
 * Pure C, no ESP-IDF dependencies: every function here is host-testable
 * (see host_test/test_easytouch.c), same split as components/rvc_protocol,
 * components/jbd_bms and components/renogy_solar.
 *
 * Protocol reference, bench captures and design rationale live in
 * docs/EASYTOUCH-THERMOSTAT.md. The short version:
 *
 *   - Three GATT characteristics on service 0x00FF: 0xDD01 password/auth,
 *     0xEE01 JSON command-in, 0xFF01 JSON status-out.
 *   - Auth is a single raw-UTF-8 write of the Micro-Air account password to
 *     0xDD01, then a read back that must start with "Matched". There is NO
 *     BLE pairing/bonding.
 *   - Everything a panel shows (per-zone mode, fan, setpoints, inside temp)
 *     is one "Get Status" write to 0xEE01 followed by a read of 0xFF01. One
 *     status carries ALL zones.
 *   - Commands are one "Change" write to 0xEE01. There is no command ACK;
 *     confirmation is the next status read, exactly like RV-C
 *     DC_DIMMER_STATUS_3.
 *
 * This file only builds/parses the JSON strings. The Bluedroid GATTC state
 * machine that carries them is easytouch_client.c (added separately) and the
 * headless gateway that runs it is the hvac/ project.
 *
 * TX side is a fixed-format snprintf — no JSON library. RX side is a
 * purpose-built bounded scanner for exactly the three shapes this protocol
 * uses ("SN":"...", "Z_sts":{"<n>":[...]}, "PRM":[...]), so host_test/
 * compiles with plain gcc like the others rather than pulling in cJSON.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* This coach's 355 reports 3 zones (Front / Mid Coach / Rear). The design's
 * wire struct allows for a 4th; keep the two in step. */
#define EASYTOUCH_MAX_ZONES 4

/* Per-zone status array length. The thermostat always sends 16 ints per
 * zone; a shorter array is treated as "zone not present" (see
 * docs/EASYTOUCH-THERMOSTAT.md §3.3, and the client's "short array" warn). */
#define EASYTOUCH_ZONE_ARRAY_LEN 16

/* Index of each field within a zone's 16-int array. Bus-confirmed layout,
 * docs/EASYTOUCH-THERMOSTAT.md §3.3 / §8a. Indices 5, 8, 13, 14 read as
 * fixed values on this unit (45, 128, 255, 0) and have no known meaning. */
#define EASYTOUCH_IDX_AUTO_HEAT_SP 0
#define EASYTOUCH_IDX_AUTO_COOL_SP 1
#define EASYTOUCH_IDX_COOL_SP      2
#define EASYTOUCH_IDX_HEAT_SP      3
#define EASYTOUCH_IDX_DRY_SP       4
#define EASYTOUCH_IDX_FAN_ONLY     6   /* fan setting in fan-only mode (0/1/2) */
#define EASYTOUCH_IDX_COOL_FAN     7
#define EASYTOUCH_IDX_AUTO_FAN     9
#define EASYTOUCH_IDX_MODE         10  /* the SELECTED mode */
#define EASYTOUCH_IDX_HEAT_FAN     11
#define EASYTOUCH_IDX_INSIDE_F     12  /* faceplate inside temperature, °F */
#define EASYTOUCH_IDX_CURRENT_MODE 15  /* what is actually running now */

/*
 * Mode enum (zone array idx 10, and the "mode" Change key). Values marked
 * "confirmed" were captured live on this coach 2026-09-06
 * (docs/EASYTOUCH-THERMOSTAT.md §8b/§8c); the rest are from the three
 * upstream open-source implementations.
 */
typedef enum {
    EASYTOUCH_MODE_OFF      = 0,   /* confirmed */
    EASYTOUCH_MODE_FAN      = 1,   /* fan only; confirmed */
    EASYTOUCH_MODE_COOL     = 2,   /* confirmed */
    EASYTOUCH_MODE_DRY      = 6,   /* upstream (as a command); never seen as status */
    EASYTOUCH_MODE_AQUA_HOT = 4,   /* confirmed — NOT the generic "heat" upstream assumed */
    EASYTOUCH_MODE_HEAT     = 7,   /* confirmed — the one ELECTRIC-heat slot; the
                                    * touchscreen labels it "Heat Pump" or "Heat
                                    * Strip" per zone, from that zone's A/C board
                                    * config, same protocol value either way */
    EASYTOUCH_MODE_AUTO     = 11,  /* upstream; not yet confirmed live on this coach */
} easytouch_mode_t;

/*
 * current_mode (idx 15) enum. On this firmware (REV 1.0.7.0) it is coarser
 * than `mode`: 0 = idle/off, 2 = actively cooling, 4 = actively heating from
 * EITHER heat source (it does not distinguish Aqua-Hot from Heat). The
 * upstream "3 = cool running" / "5 = heat running" values were never
 * observed here. See docs/EASYTOUCH-THERMOSTAT.md §8b/§8c.
 */
#define EASYTOUCH_CURRENT_IDLE    0
#define EASYTOUCH_CURRENT_COOLING 2
#define EASYTOUCH_CURRENT_HEATING 4

/*
 * Fan enum (zone array idx 7/9/11, and the coolFan/heatFan/autoFan Change
 * keys). Fan-only mode (idx 6 / fanOnly key) uses just 0/1/2.
 */
typedef enum {
    EASYTOUCH_FAN_OFF         = 0,
    EASYTOUCH_FAN_LOW         = 1,   /* manual low */
    EASYTOUCH_FAN_HIGH        = 2,   /* manual high */
    EASYTOUCH_FAN_CYCLED_LOW  = 65,  /* runs with the cycle only */
    EASYTOUCH_FAN_CYCLED_HIGH = 66,
    EASYTOUCH_FAN_AUTO        = 128, /* full auto */
} easytouch_fan_t;

/* The auth (0xDD01) readback must start with this for a good password. */
#define EASYTOUCH_AUTH_OK_PREFIX "Matched"

/* ------------------------------------------------------------ TX side --- */

/*
 * Builds the status-request JSON into out[0..out_len):
 *   {"Type":"Get Status","Zone":0}
 *
 * EM (account e-mail) and TM (Unix time) are deliberately omitted — the
 * gateway has no clock, and a bogus TM would set the thermostat's schedule
 * clock. The response is identical without them (bench-confirmed). `Zone`
 * is ignored by the thermostat for content: one response carries every zone.
 *
 * Returns the string length (excluding the NUL), or 0 if out is NULL or the
 * buffer is too small. The result is always NUL-terminated on success.
 */
size_t easytouch_build_get_status(char *out, size_t out_len);

/*
 * One pending "Change". Set the `set_*` flag for each field to emit; `zone`
 * is always emitted. Fields map 1:1 to the Change keys in
 * docs/EASYTOUCH-THERMOSTAT.md §3.4.
 *
 * ⚠️ `power` is SYSTEM-WIDE, not per-zone (it shuts down every A/C unit).
 * Per-zone off is mode = EASYTOUCH_MODE_OFF with no power key. The codec
 * emits exactly what it is told; deciding whether a power key is safe to
 * send is the gateway's job (send power:1 only when the system is currently
 * off and the user asked for a running mode).
 */
typedef struct {
    uint8_t zone;

    bool    set_power;         uint8_t power;          /* 0 / 1 — system-wide */
    bool    set_mode;          uint8_t mode;           /* easytouch_mode_t */
    bool    set_cool_sp;       uint8_t cool_sp;        /* °F */
    bool    set_heat_sp;       uint8_t heat_sp;        /* °F */
    bool    set_dry_sp;        uint8_t dry_sp;         /* °F */
    bool    set_auto_heat_sp;  uint8_t auto_heat_sp;   /* °F, AUTO low  */
    bool    set_auto_cool_sp;  uint8_t auto_cool_sp;   /* °F, AUTO high */
    bool    set_cool_fan;      uint8_t cool_fan;       /* easytouch_fan_t */
    bool    set_heat_fan;      uint8_t heat_fan;       /* easytouch_fan_t */
    bool    set_auto_fan;      uint8_t auto_fan;       /* easytouch_fan_t */
    bool    set_fan_only;      uint8_t fan_only;       /* 0 / 1 / 2 */
} easytouch_change_t;

/*
 * Builds the command JSON into out[0..out_len):
 *   {"Type":"Change","Changes":{"zone":<n>,"mode":7,...}}
 *
 * Keys are emitted in a fixed order (power, mode, the setpoints, then the
 * fans) so the output is deterministic and testable. Returns the string
 * length (excluding the NUL), or 0 if out or c is NULL, the buffer is too
 * small, or c has no set_* flag set (an empty Change is never sent).
 */
size_t easytouch_build_change(char *out, size_t out_len, const easytouch_change_t *c);

/* ------------------------------------------------------------ RX side --- */

typedef struct {
    bool    present;                        /* a full 16-int array was parsed */
    int     raw[EASYTOUCH_ZONE_ARRAY_LEN];  /* the whole array, nothing lost  */

    /* Decoded conveniences (all straight out of raw[]). */
    uint8_t mode;            /* raw[10] — easytouch_mode_t */
    uint8_t current_mode;    /* raw[15] */
    int8_t  inside_f;        /* raw[12] */
    uint8_t cool_sp;         /* raw[2]  */
    uint8_t heat_sp;         /* raw[3]  */
    uint8_t dry_sp;          /* raw[4]  */
    uint8_t auto_heat_sp;    /* raw[0]  */
    uint8_t auto_cool_sp;    /* raw[1]  */
    uint8_t fan_only;        /* raw[6]  */
    uint8_t cool_fan;        /* raw[7]  */
    uint8_t auto_fan;        /* raw[9]  */
    uint8_t heat_fan;        /* raw[11] */
} easytouch_zone_t;

typedef struct {
    char    serial[16];      /* "SN" value; empty string if absent */

    uint8_t zone_count;      /* zones with a full array, == popcount(present_mask) */
    uint8_t present_mask;    /* bit z set when zones[z].present */
    easytouch_zone_t zones[EASYTOUCH_MAX_ZONES];

    /* Raw "PRM" list, logged for the record. On firmware 1.0.7.0 the
     * upstream "7 in PRM ⇒ off / 15 in PRM ⇒ on" rule does NOT hold
     * (captured PRM was [0,107,67,76]); see docs/EASYTOUCH-THERMOSTAT.md
     * §8a/§8b. Do not derive on/off from this. */
    bool    prm_valid;
    uint8_t prm_count;
    int     prm[8];

    /* Best-effort "the system is doing something": true when any present
     * zone's current_mode != idle. This is the fallback the design settled
     * on since PRM is unreliable on this firmware. */
    bool    system_active;
} easytouch_status_t;

/*
 * Parses a "Get Status" response payload. `json` need not be NUL-terminated;
 * `len` bounds the scan. Fills *out and returns true when at least one zone
 * array was parsed out of "Z_sts". Returns false (leaving *out zeroed) on a
 * NULL argument, a missing/truncated "Z_sts", or no parseable zone.
 *
 * A truncated read (MTU too small — the failure mode the design calls out)
 * shows up here as either false or a low zone_count; callers should treat
 * "fewer zones than last time" as a bad read, not as zones disappearing.
 */
bool easytouch_parse_status(const char *json, size_t len, easytouch_status_t *out);

/* ------------------------------------------------------ display helpers - */

/*
 * Human label for a `mode` value. Returns "Heat" for EASYTOUCH_MODE_HEAT —
 * the protocol carries no Heat-Pump-vs-Heat-Strip distinction, that label
 * is per-zone on the touchscreen and a panel that wants it must carry its
 * own per-zone override (docs/EASYTOUCH-THERMOSTAT.md §8c).
 */
const char *easytouch_mode_str(uint8_t mode);

/* Human label for a fan value (idx 7/9/11 / *Fan keys). */
const char *easytouch_fan_str(uint8_t fan);

/*
 * Scan-name matcher: prefix "EasyTouch" (the advertised name is
 * "EasyTouch <serial>"). Prefix rather than exact, unlike the Renogy
 * client — a neighbouring rig's thermostat is told apart by serial and the
 * MAC is pinned after the first hit anyway. NULL-safe.
 */
bool easytouch_name_matches(const char *name);

#ifdef __cplusplus
}
#endif
