#include "jbd_bms_protocol.h"

#include <math.h>
#include <string.h>

/*
 * Checksum covers everything between the leading 0xDD and the checksum
 * field itself: for a request that's [reg, len]; for a response that's
 * [status, len, payload...]. (See the TODO(bench) note in the header —
 * this is the most commonly documented convention for this protocol, not
 * yet confirmed against a captured frame from these specific batteries.)
 */
static uint16_t jbd_checksum(const uint8_t *bytes, size_t count)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < count; i++) {
        sum += bytes[i];
    }
    return (uint16_t)(0x10000u - (sum & 0xFFFFu));
}

bool jbd_bms_build_request(uint8_t reg, uint8_t *out, size_t out_len)
{
    if (out == NULL || out_len < JBD_BMS_REQUEST_LEN) {
        return false;
    }

    const uint8_t body[2] = { reg, 0x00 };
    const uint16_t chk = jbd_checksum(body, sizeof(body));

    out[0] = JBD_BMS_START_BYTE;
    out[1] = JBD_BMS_OP_READ;
    out[2] = reg;
    out[3] = 0x00;
    out[4] = (uint8_t)(chk >> 8);
    out[5] = (uint8_t)(chk & 0xFFu);
    out[6] = JBD_BMS_END_BYTE;
    return true;
}

/*
 * 0x03 "basic info" response payload layout (offsets within the payload,
 * i.e. relative to frame[4]):
 *   0-1   total voltage, 0.01 V, uint16 BE
 *   2-3   current, 0.01 A, int16 BE (signed; see sign-convention TODO)
 *   4-5   residual capacity, 0.01 Ah, uint16 BE
 *   6-7   nominal/full capacity, 0.01 Ah, uint16 BE
 *   8-9   cycle count, uint16 BE
 *   10-18 production date, balance status, protection status, sw version
 *         (unused here)
 *   19    RSOC -- state of charge percent, uint8 (used directly, not
 *         recomputed from the two capacity fields)
 *   20+   FET status, string count, NTC count, NTC temps (unused here)
 * Minimum payload length trusted: 20 bytes (through the RSOC byte).
 */
#define JBD_BASIC_INFO_MIN_PAYLOAD 20u

bool jbd_bms_parse_basic_info(const uint8_t *frame, size_t len, jbd_bms_status_t *out)
{
    if (frame == NULL || out == NULL || len < 7) {
        return false;
    }
    if (frame[0] != JBD_BMS_START_BYTE || frame[len - 1] != JBD_BMS_END_BYTE) {
        return false;
    }
    if (frame[1] != JBD_BMS_REG_BASIC_INFO) {
        return false;
    }

    const uint8_t status = frame[2];
    const uint8_t payload_len = frame[3];
    if (status != 0x00) {
        return false;   /* BMS reported an error for this register */
    }
    /* Frame must hold: DD REG STATUS LEN <payload_len bytes> CHKH CHKL 77 */
    if (len < (size_t)(4 + payload_len + 3)) {
        return false;
    }
    if (payload_len < JBD_BASIC_INFO_MIN_PAYLOAD) {
        return false;
    }

    const uint8_t *payload = &frame[4];
    const uint16_t got_chk = (uint16_t)((frame[4 + payload_len] << 8) | frame[5 + payload_len]);
    uint8_t chk_body[2 + 64];
    if ((size_t)(2 + payload_len) > sizeof(chk_body)) {
        return false;
    }
    chk_body[0] = status;
    chk_body[1] = payload_len;
    memcpy(&chk_body[2], payload, payload_len);
    if (jbd_checksum(chk_body, (size_t)(2 + payload_len)) != got_chk) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->voltage_v = (float)((payload[0] << 8) | payload[1]) * 0.01f;
    out->current_a = (float)(int16_t)((payload[2] << 8) | payload[3]) * 0.01f;
    out->residual_ah = (float)((payload[4] << 8) | payload[5]) * 0.01f;
    out->full_capacity_ah = (float)((payload[6] << 8) | payload[7]) * 0.01f;
    out->cycles = (uint16_t)((payload[8] << 8) | payload[9]);
    out->soc_percent = payload[19];
    return true;
}

float jbd_bms_estimate_hours(const jbd_bms_status_t *s)
{
    /* Below this, treat the pack as idle -- a fractional-amp reading
     * shouldn't produce a wild multi-thousand-hour estimate. */
    const float idle_threshold_a = 0.05f;

    if (s->current_a > idle_threshold_a) {
        /* Charging: time until full. */
        const float remaining_ah = s->full_capacity_ah - s->residual_ah;
        if (remaining_ah <= 0.0f) {
            return INFINITY;
        }
        return remaining_ah / s->current_a;
    }
    if (s->current_a < -idle_threshold_a) {
        /* Discharging: time until empty. */
        return s->residual_ah / -s->current_a;
    }
    return INFINITY;
}
