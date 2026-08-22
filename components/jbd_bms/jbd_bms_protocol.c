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
 *   20    FET status (unused here)
 *   21    cell/string count (unused here)
 *   22    NTC probe count
 *   23+   NTC raw values, uint16 BE, 2 bytes each; degC = (raw - 2731)/10
 *
 * Minimum payload length trusted: 20 bytes (through the RSOC byte) -- a
 * shorter-than-expected payload still yields voltage/current/SOC, it just
 * reports temp_count == 0. Temperatures are only read when the payload
 * actually extends far enough to hold the count byte's worth of values.
 *
 * NB: a secondary source places the NTC count at offset 21 rather than 22.
 * The captured frame in host_test rules that out -- offset 21 there is 0x0F
 * (15 cells), and 15 probes would need a 53-byte payload where the frame
 * carries 27.
 */
#define JBD_BASIC_INFO_MIN_PAYLOAD 20u
#define JBD_NTC_COUNT_OFFSET       22u
#define JBD_NTC_FIRST_OFFSET       23u
#define JBD_NTC_ZERO_C_DECIKELVIN  2731

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

    /* Temperatures, only if the payload really carries them. */
    if (payload_len > JBD_NTC_COUNT_OFFSET) {
        uint8_t ntc = payload[JBD_NTC_COUNT_OFFSET];
        if (ntc > JBD_BMS_MAX_TEMPS) {
            ntc = JBD_BMS_MAX_TEMPS;
        }
        if ((size_t)payload_len >= (size_t)JBD_NTC_FIRST_OFFSET + 2u * ntc) {
            for (uint8_t i = 0; i < ntc; i++) {
                const size_t at = JBD_NTC_FIRST_OFFSET + 2u * i;
                const int raw = (int)((payload[at] << 8) | payload[at + 1]);
                out->temp_c[i] = (float)(raw - JBD_NTC_ZERO_C_DECIKELVIN) * 0.1f;
            }
            out->temp_count = ntc;
        }
    }
    return true;
}

float jbd_bms_power_w(const jbd_bms_status_t *s)
{
    return s->voltage_v * s->current_a;
}

float jbd_bms_c_to_f(float celsius)
{
    return celsius * 1.8f + 32.0f;
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

bool jbd_bms_combine(const jbd_bms_status_t *packs, uint8_t count,
                     jbd_bms_bank_t *out)
{
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->hours = INFINITY;
    if (packs == NULL || count == 0) {
        return false;
    }

    float voltage_sum = 0.0f;
    float soc_weighted_sum = 0.0f;   /* sum of soc_i * full_ah_i */

    for (uint8_t i = 0; i < count; i++) {
        const jbd_bms_status_t *p = &packs[i];
        voltage_sum += p->voltage_v;
        out->current_a += p->current_a;
        /* Sum per-pack V*I rather than mean_V * sum_I: identical while the
         * packs agree, but stays right if one reads a different voltage. */
        out->power_w += jbd_bms_power_w(p);
        out->residual_ah += p->residual_ah;
        out->full_capacity_ah += p->full_capacity_ah;
        soc_weighted_sum += (float)p->soc_percent * p->full_capacity_ah;

        for (uint8_t t = 0; t < p->temp_count && t < JBD_BMS_MAX_TEMPS; t++) {
            const float c = p->temp_c[t];
            if (!out->temp_valid) {
                out->temp_valid = true;
                out->temp_min_c = c;
                out->temp_max_c = c;
            }
            if (c < out->temp_min_c) {
                out->temp_min_c = c;
            }
            if (c > out->temp_max_c) {
                out->temp_max_c = c;
            }
        }
    }

    out->pack_count = count;
    out->voltage_v = voltage_sum / (float)count;

    /* Capacity-weighted mean of each BMS's own RSOC. For equal packs this is
     * a plain mean; it stays correct if a pack drops out of the bank or is
     * later replaced with a different capacity. Falls back to an unweighted
     * mean if the packs report no capacity at all (shouldn't happen, but a
     * zero divisor would otherwise produce NaN on screen). */
    if (out->full_capacity_ah > 0.0f) {
        out->soc_percent = (uint8_t)((soc_weighted_sum / out->full_capacity_ah) + 0.5f);
    } else {
        float soc_sum = 0.0f;
        for (uint8_t i = 0; i < count; i++) {
            soc_sum += (float)packs[i].soc_percent;
        }
        out->soc_percent = (uint8_t)((soc_sum / (float)count) + 0.5f);
    }
    if (out->soc_percent > 100) {
        out->soc_percent = 100;
    }

    /* Time remaining is computed from the aggregate, not averaged from
     * per-pack estimates -- reuse the single-pack estimator by handing it
     * the bank totals. */
    const jbd_bms_status_t aggregate = {
        .current_a = out->current_a,
        .residual_ah = out->residual_ah,
        .full_capacity_ah = out->full_capacity_ah,
    };
    out->hours = jbd_bms_estimate_hours(&aggregate);
    return true;
}
