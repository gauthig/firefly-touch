#include "firefly_dgn.h"

#include <math.h>
#include <string.h>

/* ---------------------------------------------------------- helpers ---- */

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

/* Unsigned scaled field: round to the nearest step, saturate at [0, max_valid],
 * NAN -> not available. max_valid stays below the n/a code so it never aliases. */
static uint16_t enc_u16(float v, float step, uint16_t max_valid)
{
    if (isnan(v)) {
        return FDGN_U16_NA;
    }
    float r = roundf(v / step);
    if (r < 0.0f) {
        r = 0.0f;
    }
    if (r > (float)max_valid) {
        r = (float)max_valid;
    }
    return (uint16_t)r;
}

static float dec_u16(uint16_t raw, float step)
{
    return (raw == FDGN_U16_NA) ? NAN : (float)raw * step;
}

/* Signed scaled field: saturate at [-32768, 32766] so 0x7FFF stays "n/a". */
static uint16_t enc_s16(float v, float step)
{
    if (isnan(v)) {
        return (uint16_t)FDGN_S16_NA;
    }
    float r = roundf(v / step);
    if (r < -32768.0f) {
        r = -32768.0f;
    }
    if (r > 32766.0f) {
        r = 32766.0f;
    }
    return (uint16_t)(int16_t)r;
}

static float dec_s16(uint16_t raw, float step)
{
    const int16_t s = (int16_t)raw;
    return (s == FDGN_S16_NA) ? NAN : (float)s * step;
}

/* Compact temperature: C = raw * 0.5 - 40, raw 0..254, 0xFF n/a. */
static uint8_t enc_temp_c(float c)
{
    if (isnan(c)) {
        return FDGN_U8_NA;
    }
    float r = roundf((c + 40.0f) * 2.0f);
    if (r < 0.0f) {
        r = 0.0f;
    }
    if (r > 254.0f) {
        r = 254.0f;
    }
    return (uint8_t)r;
}

static float dec_temp_c(uint8_t raw)
{
    return (raw == FDGN_U8_NA) ? NAN : (float)raw * 0.5f - 40.0f;
}

/* Solar temperatures: F = raw - 40, raw 0..254, 0xFF n/a. */
static uint8_t enc_temp_f(float f)
{
    if (isnan(f)) {
        return FDGN_U8_NA;
    }
    float r = roundf(f + 40.0f);
    if (r < 0.0f) {
        r = 0.0f;
    }
    if (r > 254.0f) {
        r = 254.0f;
    }
    return (uint8_t)r;
}

static float dec_temp_f(uint8_t raw)
{
    return (raw == FDGN_U8_NA) ? NAN : (float)raw - 40.0f;
}

/* Shore frequency: Hz = 40.0 + raw * 0.1, raw 0..254, 0xFF n/a. */
static uint8_t enc_hz(float hz)
{
    if (isnan(hz)) {
        return FDGN_U8_NA;
    }
    float r = roundf((hz - 40.0f) * 10.0f);
    if (r < 0.0f) {
        r = 0.0f;
    }
    if (r > 254.0f) {
        r = 254.0f;
    }
    return (uint8_t)r;
}

static float dec_hz(uint8_t raw)
{
    return (raw == FDGN_U8_NA) ? NAN : 40.0f + (float)raw * 0.1f;
}

static uint8_t sat254(uint32_t v)
{
    return (uint8_t)((v > 254u) ? 254u : v);
}

static uint16_t sat_u16_valid(uint32_t v)
{
    return (uint16_t)((v > 0xFFFEu) ? 0xFFFEu : v);
}

static bool frame_ok(const uint8_t *d, uint8_t dlc)
{
    return d != NULL && dlc == FDGN_LEN;
}

/* ----------------------------------------------------------- block ----- */

bool fdgn_is_ours(uint32_t dgn)
{
    return dgn >= FDGN_FIRST && dgn <= FDGN_LAST;
}

uint32_t fdgn_pack_dgn(uint8_t pack)
{
    return (pack >= 1u && pack <= 3u) ? (uint32_t)FDGN_BATT_PACK1 + pack - 1u : 0u;
}

uint8_t fdgn_pack_from_dgn(uint32_t dgn)
{
    return (dgn >= FDGN_BATT_PACK1 && dgn <= FDGN_BATT_PACK3)
               ? (uint8_t)(dgn - FDGN_BATT_PACK1 + 1u) : 0u;
}

/* ------------------------------------------------------------- HVAC ---- */

void fdgn_encode_hvac_status(const fdgn_hvac_status_t *in, uint8_t out[FDGN_LEN])
{
    out[0] = in->zone;
    out[1] = (uint8_t)((in->present ? 0x01u : 0u) | (in->fresh ? 0x02u : 0u));
    out[2] = in->mode;
    out[3] = in->current_mode;
    out[4] = in->fan;
    out[5] = (uint8_t)in->inside_f;
    out[6] = in->cool_sp_f;
    out[7] = in->heat_sp_f;
}

bool fdgn_decode_hvac_status(const uint8_t *d, uint8_t dlc, fdgn_hvac_status_t *out)
{
    if (!frame_ok(d, dlc) || d[0] > 3u) {
        return false;
    }
    out->zone = d[0];
    out->present = (d[1] & 0x01u) != 0;
    out->fresh = (d[1] & 0x02u) != 0;
    out->mode = d[2];
    out->current_mode = d[3];
    out->fan = d[4];
    out->inside_f = (int8_t)d[5];
    out->cool_sp_f = d[6];
    out->heat_sp_f = d[7];
    return true;
}

void fdgn_encode_hvac_extras(const fdgn_hvac_extras_t *in, uint8_t out[FDGN_LEN])
{
    out[0] = in->zone;
    out[1] = in->auto_heat_sp_f;
    out[2] = in->auto_cool_sp_f;
    out[3] = (in->age_s == FDGN_U8_NA) ? FDGN_U8_NA : sat254(in->age_s);
    out[4] = in->fan_only;
    out[5] = in->cool_fan;
    out[6] = in->heat_fan;
    out[7] = in->auto_fan;
}

bool fdgn_decode_hvac_extras(const uint8_t *d, uint8_t dlc, fdgn_hvac_extras_t *out)
{
    if (!frame_ok(d, dlc) || d[0] > 3u) {
        return false;
    }
    out->zone = d[0];
    out->auto_heat_sp_f = d[1];
    out->auto_cool_sp_f = d[2];
    out->age_s = d[3];
    out->fan_only = d[4];
    out->cool_fan = d[5];
    out->heat_fan = d[6];
    out->auto_fan = d[7];
    return true;
}

void fdgn_encode_hvac_cmd(const fdgn_hvac_cmd_t *in, uint8_t out[FDGN_LEN])
{
    memset(out, 0xFF, FDGN_LEN);
    out[0] = in->zone;
    out[1] = in->op;
    out[2] = in->arg;
}

bool fdgn_decode_hvac_cmd(const uint8_t *d, uint8_t dlc, fdgn_hvac_cmd_t *out)
{
    /* A command is only acted on if every field is meaningful. */
    if (!frame_ok(d, dlc) || d[0] > 3u || d[1] > FDGN_HVAC_OP_FAN_ONLY) {
        return false;
    }
    out->zone = d[0];
    out->op = d[1];
    out->arg = d[2];
    return true;
}

/* ---------------------------------------------------------- Battery ---- */

void fdgn_encode_batt_pack(const fdgn_batt_pack_t *in, uint8_t out[FDGN_LEN])
{
    out[0] = in->pack;
    out[1] = (uint8_t)((in->online ? 0x01u : 0u) | (in->temps_valid ? 0x02u : 0u));
    out[2] = in->soc_pct;
    put_u16(&out[3], enc_u16(in->volts, 0.01f, 0xFFFEu));
    put_u16(&out[5], enc_s16(in->amps, 0.1f));
    out[7] = 0xFFu;
}

bool fdgn_decode_batt_pack(const uint8_t *d, uint8_t dlc, fdgn_batt_pack_t *out)
{
    if (!frame_ok(d, dlc) || d[0] < 1u || d[0] > 3u) {
        return false;
    }
    out->pack = d[0];
    out->online = (d[1] & 0x01u) != 0;
    out->temps_valid = (d[1] & 0x02u) != 0;
    out->soc_pct = d[2];
    out->volts = dec_u16(get_u16(&d[3]), 0.01f);
    out->amps = dec_s16(get_u16(&d[5]), 0.1f);
    return true;
}

void fdgn_encode_pack_detail(const fdgn_pack_detail_t *in, uint8_t out[FDGN_LEN])
{
    out[0] = in->pack;
    put_u16(&out[1], enc_u16(in->residual_ah, 0.1f, 0xFFFEu));
    put_u16(&out[3], enc_u16(in->full_ah, 0.1f, 0xFFFEu));
    out[5] = enc_temp_c(in->temp_min_c);
    out[6] = enc_temp_c(in->temp_max_c);
    out[7] = 0xFFu;
}

bool fdgn_decode_pack_detail(const uint8_t *d, uint8_t dlc, fdgn_pack_detail_t *out)
{
    if (!frame_ok(d, dlc) || d[0] < 1u || d[0] > 3u) {
        return false;
    }
    out->pack = d[0];
    out->residual_ah = dec_u16(get_u16(&d[1]), 0.1f);
    out->full_ah = dec_u16(get_u16(&d[3]), 0.1f);
    out->temp_min_c = dec_temp_c(d[5]);
    out->temp_max_c = dec_temp_c(d[6]);
    return true;
}

void fdgn_encode_bank_a(const fdgn_bank_a_t *in, uint8_t out[FDGN_LEN])
{
    const uint8_t cfg = (in->packs_configured > 15u) ? 15u : in->packs_configured;
    const uint8_t onl = (in->packs_online > 15u) ? 15u : in->packs_online;
    out[0] = in->bank;
    out[1] = (uint8_t)((cfg << 4) | onl);
    out[2] = in->soc_pct;
    put_u16(&out[3], enc_u16(in->volts, 0.01f, 0xFFFEu));
    put_u16(&out[5], enc_s16(in->amps, 0.1f));
    out[7] = 0xFFu;
}

bool fdgn_decode_bank_a(const uint8_t *d, uint8_t dlc, fdgn_bank_a_t *out)
{
    if (!frame_ok(d, dlc) || d[0] == 0u || d[0] == FDGN_U8_NA) {
        return false;
    }
    out->bank = d[0];
    out->packs_configured = (uint8_t)(d[1] >> 4);
    out->packs_online = (uint8_t)(d[1] & 0x0Fu);
    out->soc_pct = d[2];
    out->volts = dec_u16(get_u16(&d[3]), 0.01f);
    out->amps = dec_s16(get_u16(&d[5]), 0.1f);
    return true;
}

void fdgn_encode_bank_b(const fdgn_bank_b_t *in, uint8_t out[FDGN_LEN])
{
    out[0] = in->bank;
    put_u16(&out[1], enc_s16(in->watts, 1.0f));
    put_u16(&out[3], (in->minutes_left == FDGN_U16_NA) ? FDGN_U16_NA
                                                        : sat_u16_valid(in->minutes_left));
    out[5] = enc_temp_c(in->temp_high_c);
    out[6] = enc_temp_c(in->temp_low_c);
    out[7] = in->state;
}

bool fdgn_decode_bank_b(const uint8_t *d, uint8_t dlc, fdgn_bank_b_t *out)
{
    if (!frame_ok(d, dlc) || d[0] == 0u || d[0] == FDGN_U8_NA) {
        return false;
    }
    out->bank = d[0];
    out->watts = dec_s16(get_u16(&d[1]), 1.0f);
    out->minutes_left = get_u16(&d[3]);
    out->temp_high_c = dec_temp_c(d[5]);
    out->temp_low_c = dec_temp_c(d[6]);
    out->state = d[7];
    return true;
}

/* ------------------------------------------------------------ Shore ---- */

void fdgn_encode_shore_line(const fdgn_shore_line_t *in, uint8_t out[FDGN_LEN])
{
    out[0] = in->line;
    put_u16(&out[1], enc_u16(in->volts, 0.1f, 0xFFFEu));
    put_u16(&out[3], enc_u16(in->amps, 0.1f, 0xFFFEu));
    put_u16(&out[5], enc_u16(in->watts, 1.0f, 0xFFFEu));
    out[7] = enc_hz(in->hz);
}

bool fdgn_decode_shore_line(const uint8_t *d, uint8_t dlc, fdgn_shore_line_t *out)
{
    if (!frame_ok(d, dlc) || d[0] > 1u) {
        return false;
    }
    out->line = d[0];
    out->volts = dec_u16(get_u16(&d[1]), 0.1f);
    out->amps = dec_u16(get_u16(&d[3]), 0.1f);
    out->watts = dec_u16(get_u16(&d[5]), 1.0f);
    out->hz = dec_hz(d[7]);
    return true;
}

void fdgn_encode_shore_pedestal(const fdgn_shore_pedestal_t *in, uint8_t out[FDGN_LEN])
{
    memset(out, 0xFF, FDGN_LEN);
    out[0] = 0u;
    out[1] = in->line_count;
    out[2] = in->error_code;
}

bool fdgn_decode_shore_pedestal(const uint8_t *d, uint8_t dlc, fdgn_shore_pedestal_t *out)
{
    if (!frame_ok(d, dlc) || d[0] != 0u) {
        return false;
    }
    out->line_count = d[1];
    out->error_code = d[2];
    return true;
}

/* ------------------------------------------------------------ Solar ---- */

void fdgn_encode_solar_a(const fdgn_solar_a_t *in, uint8_t out[FDGN_LEN])
{
    out[0] = in->controller;
    out[1] = in->online ? 0x01u : 0u;
    out[2] = in->charge_state;
    out[3] = in->battery_soc_pct;
    put_u16(&out[4], enc_u16(in->battery_volts, 0.01f, 0xFFFEu));
    put_u16(&out[6], enc_u16(in->pv_volts, 0.01f, 0xFFFEu));
}

bool fdgn_decode_solar_a(const uint8_t *d, uint8_t dlc, fdgn_solar_a_t *out)
{
    if (!frame_ok(d, dlc) || d[0] == 0u || d[0] == FDGN_U8_NA) {
        return false;
    }
    out->controller = d[0];
    out->online = (d[1] & 0x01u) != 0;
    out->charge_state = d[2];
    out->battery_soc_pct = d[3];
    out->battery_volts = dec_u16(get_u16(&d[4]), 0.01f);
    out->pv_volts = dec_u16(get_u16(&d[6]), 0.01f);
    return true;
}

void fdgn_encode_solar_b(const fdgn_solar_b_t *in, uint8_t out[FDGN_LEN])
{
    out[0] = in->controller;
    put_u16(&out[1], enc_u16(in->pv_amps, 0.01f, 0xFFFEu));
    put_u16(&out[3], enc_u16(in->pv_watts, 1.0f, 0xFFFEu));
    out[5] = enc_temp_f(in->controller_temp_f);
    out[6] = enc_temp_f(in->battery_temp_f);
    out[7] = 0xFFu;
}

bool fdgn_decode_solar_b(const uint8_t *d, uint8_t dlc, fdgn_solar_b_t *out)
{
    if (!frame_ok(d, dlc) || d[0] == 0u || d[0] == FDGN_U8_NA) {
        return false;
    }
    out->controller = d[0];
    out->pv_amps = dec_u16(get_u16(&d[1]), 0.01f);
    out->pv_watts = dec_u16(get_u16(&d[3]), 1.0f);
    out->controller_temp_f = dec_temp_f(d[5]);
    out->battery_temp_f = dec_temp_f(d[6]);
    return true;
}

/* ----------------------------------------------------------- Valves ---- */

void fdgn_encode_valve_status(const fdgn_valve_status_t *in, uint8_t out[FDGN_LEN])
{
    memset(out, 0xFF, FDGN_LEN);
    out[0] = in->valve;
    out[1] = in->position;
    out[2] = (in->age_s == FDGN_U8_NA) ? FDGN_U8_NA : sat254(in->age_s);
    out[3] = in->node_fresh ? 0x01u : 0u;
}

bool fdgn_decode_valve_status(const uint8_t *d, uint8_t dlc, fdgn_valve_status_t *out)
{
    if (!frame_ok(d, dlc) || d[0] > 1u) {
        return false;
    }
    out->valve = d[0];
    out->position = (d[1] > FDGN_VALVE_OPEN) ? (uint8_t)FDGN_VALVE_UNKNOWN : d[1];
    out->age_s = d[2];
    out->node_fresh = (d[3] & 0x01u) != 0;
    return true;
}

void fdgn_encode_valve_cmd(const fdgn_valve_cmd_t *in, uint8_t out[FDGN_LEN])
{
    memset(out, 0xFF, FDGN_LEN);
    out[0] = in->valve;
    out[1] = in->action;
}

bool fdgn_decode_valve_cmd(const uint8_t *d, uint8_t dlc, fdgn_valve_cmd_t *out)
{
    /* A dump valve moves on this, so only an exact, in-range frame is accepted. */
    if (!frame_ok(d, dlc) || d[0] > 1u || d[1] > FDGN_VALVE_ACT_OPEN) {
        return false;
    }
    out->valve = d[0];
    out->action = d[1];
    return true;
}

/* -------------------------------------------------------- Heartbeat ---- */

void fdgn_encode_heartbeat(const fdgn_heartbeat_t *in, uint8_t out[FDGN_LEN])
{
    out[0] = in->gateway_id;
    out[1] = in->fw_major;
    out[2] = in->fw_minor;
    put_u16(&out[3], sat_u16_valid(in->uptime_min));
    out[5] = in->links;
    out[6] = in->health;
    out[7] = 0xFFu;
}

bool fdgn_decode_heartbeat(const uint8_t *d, uint8_t dlc, fdgn_heartbeat_t *out)
{
    if (!frame_ok(d, dlc) || d[0] == FDGN_U8_NA) {
        return false;
    }
    out->gateway_id = d[0];
    out->fw_major = d[1];
    out->fw_minor = d[2];
    out->uptime_min = get_u16(&d[3]);
    out->links = d[5];
    out->health = d[6];
    return true;
}
