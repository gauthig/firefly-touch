/*
 * jbd_bms_protocol — Xiaoxiang/JBD Smart BMS UART-over-BLE frame codec.
 *
 * Pure C, no ESP-IDF dependencies: every function here is host-testable
 * (see host_test/test_jbd_bms.c), same split as components/rvc_protocol.
 *
 * Frame layout (JBD/Xiaoxiang protocol, used identically over UART and BLE
 * notify/write characteristics):
 *   request:  DD A5 <reg> 00 <chk_hi> <chk_lo> 77                 (7 bytes)
 *   response: DD <reg> <status> <len> <payload[len]> <chk_hi> <chk_lo> 77
 * checksum = two's complement (mod 0x10000) of the sum of every byte
 * between the leading 0xDD and the checksum field itself.
 *
 * Byte layout and checksum scope are verified against a real published JBD
 * response frame (see host_test/test_jbd_bms.c, k_jbd_doc_frame): its own
 * trailing checksum reproduces exactly under jbd_checksum(), 58.88 V / 15
 * cells = 3.92 V/cell, and its 27-byte payload is exactly 23 + 2*2, i.e.
 * the two NTC values its temperature-count byte advertises. Treat the
 * offsets below as confirmed.
 *
 * TODO(bench): the CURRENT SIGN CONVENTION is still an assumption —
 * positive = charging, negative = discharging. Nothing in the captured
 * frame pins it down (its current field is zero). Confirm against a pack
 * that is visibly charging vs. discharging and flip
 * jbd_bms_estimate_hours()'s branches if these packs disagree.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JBD_BMS_START_BYTE      0xDDu
#define JBD_BMS_END_BYTE        0x77u
#define JBD_BMS_OP_READ         0xA5u
#define JBD_BMS_REG_BASIC_INFO  0x03u

/* Request frames are always exactly this many bytes. */
#define JBD_BMS_REQUEST_LEN 7u

/* JBD supports up to 6 NTC probes; the Vatrer 300 Ah packs report 2. */
#define JBD_BMS_MAX_TEMPS 6u

/*
 * Builds a read-request frame for `reg` into out[0..out_len). Returns false
 * if out_len < JBD_BMS_REQUEST_LEN.
 */
bool jbd_bms_build_request(uint8_t reg, uint8_t *out, size_t out_len);

typedef struct {
    float    voltage_v;
    /* Positive = charging, negative = discharging. Sign convention is the
     * TODO(bench) item noted above — flip if a real pack disagrees. */
    float    current_a;
    float    residual_ah;
    float    full_capacity_ah;
    uint16_t cycles;
    uint8_t  soc_percent;   /* RSOC byte, 0..100 */
    /* NTC probe readings. temp_count is 0 when the frame was too short to
     * carry them (shorter basic-info payloads still parse fine —
     * callers display "--" rather than a bogus temperature). */
    uint8_t  temp_count;
    float    temp_c[JBD_BMS_MAX_TEMPS];
} jbd_bms_status_t;

/*
 * Parses a complete 0x03 "basic info" response frame (start byte through
 * end byte inclusive). Returns false if too short, malformed (bad
 * start/end byte or echoed register), or the checksum doesn't match.
 */
bool jbd_bms_parse_basic_info(const uint8_t *frame, size_t len, jbd_bms_status_t *out);

/*
 * Estimated hours remaining at the current rate: residual/|current| while
 * discharging, (full - residual)/current while charging. Returns INFINITY
 * for a near-zero (idle) current -- callers display that as "--".
 */
float jbd_bms_estimate_hours(const jbd_bms_status_t *s);

/* Instantaneous pack power. The protocol carries no watts field, so this is
 * always derived: V * A, signed the same way current_a is. */
float jbd_bms_power_w(const jbd_bms_status_t *s);

/* Display helper -- the panel shows temperatures in Fahrenheit. */
float jbd_bms_c_to_f(float celsius);

/*
 * One combined reading for a set of packs wired in PARALLEL (this coach:
 * three identical Vatrer 300 Ah packs bus-tied together).
 *
 * There is no BMS-side aggregation to read instead: a JBD BLE module is a
 * UART bridge to one BMS and knows nothing of its siblings. (Vendor displays
 * that show a whole bank from one connection use the packs' RS485
 * inter-pack daisy-chain, a different bus this panel isn't wired into.) So
 * the combining happens here.
 */
typedef struct {
    uint8_t pack_count;         /* packs contributing; 0 == nothing valid */
    float   voltage_v;          /* mean -- packs are hard-tied, so averaging
                                     just cancels per-BMS shunt/ADC offset */
    float   current_a;          /* sum, signed */
    float   power_w;            /* sum of per-pack V*I, signed */
    float   residual_ah;        /* sum */
    float   full_capacity_ah;   /* sum */
    uint8_t soc_percent;        /* capacity-weighted mean of each BMS's RSOC */
    float   hours;              /* from the aggregate; INFINITY when idle */
    bool    temp_valid;         /* false if no pack reported any NTC */
    float   temp_min_c;
    float   temp_max_c;
} jbd_bms_bank_t;

/*
 * Combines `count` pack readings into one bank reading. `packs` must be a
 * PACKED array of currently-valid readings only -- callers filter offline
 * packs out beforehand, so a dropped pack shrinks the bank rather than
 * poisoning the averages.
 *
 * Returns false (and zeroes *out) when count == 0 or out == NULL.
 */
bool jbd_bms_combine(const jbd_bms_status_t *packs, uint8_t count,
                     jbd_bms_bank_t *out);

#ifdef __cplusplus
}
#endif
