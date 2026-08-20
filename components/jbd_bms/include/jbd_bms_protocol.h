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
 * Byte layout cross-checked against publicly documented JBD/Xiaoxiang BMS
 * implementations (e.g. ESPHome's jbd_bms component and other open BMS
 * bridges), the same way rvc_protocol.c cross-checks DGN encoding against
 * rvc-proxy/rvc2hass. TODO(bench): verify field offsets, the checksum
 * scope, and the current sign convention (charge vs. discharge) against a
 * real captured notify frame once hardware is available — do not trust
 * these byte offsets blindly the way the RV-C ones were trusted before
 * being sniffer-verified.
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

#ifdef __cplusplus
}
#endif
