/* See renogy_solar_protocol.h for the register map, the CRC convention and
 * the temperature sign-flag trap. */
#include "renogy_solar_protocol.h"

#include <string.h>

/*
 * Bit-by-bit rather than a 512-byte lookup table: this runs once per poll
 * interval (30 s by default) over 73 bytes, so the table would buy nothing
 * measurable and cost flash on a 4 MB part already carrying Bluedroid.
 */
uint16_t renogy_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;

    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0xA001u) : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

/*
 * Stand-alone first, then the daisy-chain and Communication-Hub addresses.
 * See the header: a wrong id produces silence, not an error, so the client
 * walks this list rather than sitting there looking dead.
 */
const uint8_t k_renogy_device_id_candidates[] = { 255u, 17u, 16u, 96u, 97u, 1u };
const size_t  k_renogy_device_id_candidate_count =
    sizeof(k_renogy_device_id_candidates) / sizeof(k_renogy_device_id_candidates[0]);

size_t renogy_build_read_request(uint8_t device_id, uint16_t reg, uint16_t words,
                                 uint8_t *out)
{
    if (out == NULL) {
        return 0;
    }

    out[0] = device_id;
    out[1] = RENOGY_FUNC_READ;
    out[2] = (uint8_t)(reg >> 8);
    out[3] = (uint8_t)(reg & 0xFFu);
    out[4] = (uint8_t)(words >> 8);
    out[5] = (uint8_t)(words & 0xFFu);

    const uint16_t crc = renogy_crc16(out, 6);
    /* Modbus RTU puts the CRC on the wire low byte first. Getting this
     * backwards is accepted by nothing and answered by nothing. */
    out[6] = (uint8_t)(crc & 0xFFu);
    out[7] = (uint8_t)(crc >> 8);

    return RENOGY_REQUEST_LEN;
}

size_t renogy_expected_response_len(uint16_t words)
{
    return (size_t)words * 2u + RENOGY_RESP_OVERHEAD;
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

float renogy_decode_temp_c(uint8_t raw)
{
    /* Sign FLAG, not two's complement -- see the header. */
    if (raw & 0x80u) {
        return -(float)(raw & 0x7Fu);
    }
    return (float)raw;
}

float renogy_c_to_f(float celsius)
{
    return celsius * 9.0f / 5.0f + 32.0f;
}

bool renogy_parse_charging_info(const uint8_t *data, size_t len,
                                renogy_solar_status_t *out)
{
    if (data == NULL || out == NULL || len != RENOGY_CHARGING_RESP_LEN) {
        return false;
    }
    /* An exception reply (function | 0x80) is shorter than this, so it can
     * only get here as a corrupt frame -- but check anyway, since a caller
     * reassembling by expected length would otherwise parse the exception
     * code as a battery reading. */
    if (data[1] != RENOGY_FUNC_READ) {
        return false;
    }
    if (data[2] != (uint8_t)(RENOGY_CHARGING_WORDS * 2u)) {
        return false;
    }

    /* CRC covers everything but the two CRC bytes themselves, and is on the
     * wire low byte first. */
    const uint16_t want = renogy_crc16(data, len - 2u);
    const uint16_t got = (uint16_t)(data[len - 2u] | ((uint16_t)data[len - 1u] << 8));
    if (want != got) {
        return false;
    }

    memset(out, 0, sizeof(*out));

    /* Byte 0 is the controller's REAL address even when 255 was asked for,
     * which is what makes the device-id probe self-correcting. */
    out->device_id = data[0];

    out->battery_soc       = (uint8_t)be16(&data[3]);
    out->battery_voltage_v = (float)be16(&data[5]) * 0.1f;
    out->battery_current_a = (float)be16(&data[7]) * 0.01f;

    out->controller_temp_c = renogy_decode_temp_c(data[9]);
    out->battery_temp_c    = renogy_decode_temp_c(data[10]);

    out->load_voltage_v = (float)be16(&data[11]) * 0.1f;
    out->load_current_a = (float)be16(&data[13]) * 0.01f;
    out->load_power_w   = be16(&data[15]);

    out->pv_voltage_v = (float)be16(&data[17]) * 0.1f;
    out->pv_current_a = (float)be16(&data[19]) * 0.01f;
    out->pv_power_w   = be16(&data[21]);

    out->load_on      = (data[67] & 0x80u) != 0u;
    out->charge_state = data[68];

    return true;
}

bool renogy_name_matches(const char *name)
{
    /*
     * Prefix match, deliberately -- unlike hughes_wd_name_matches(), which
     * has to match a SUBSTRING because this coach's Watchdog advertises as
     * "APMD..." with a leading A. Renogy's modules are consistent about
     * their prefixes, and a substring test here would let a device merely
     * containing "BT-1" through.
     */
    static const char *const k_prefixes[] = {
        "BT-TH",     /* BT-1 / BT-2 modules -- the charge controllers */
        "BT-1",
        "BT-2",
        "RNGRBP",    /* smart batteries */
        "RNGRIU",
        "BTRIC",     /* inverter/chargers */
    };

    if (name == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(k_prefixes) / sizeof(k_prefixes[0]); i++) {
        const size_t n = strlen(k_prefixes[i]);
        if (strncmp(name, k_prefixes[i], n) == 0) {
            return true;
        }
    }
    return false;
}

/* Length of `s` ignoring any trailing whitespace. */
static size_t trimmed_len(const char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                     s[n - 1] == '\r' || s[n - 1] == '\n')) {
        n--;
    }
    return n;
}

bool renogy_name_equals(const char *adv_name, const char *want)
{
    if (adv_name == NULL || want == NULL) {
        return false;
    }
    const size_t a = trimmed_len(adv_name);
    const size_t b = trimmed_len(want);
    return a == b && strncmp(adv_name, want, a) == 0;
}

const char *renogy_charge_state_str(uint8_t state)
{
    switch (state) {
    case RENOGY_CHARGE_DEACTIVATED: return "off";
    case RENOGY_CHARGE_ACTIVATED:   return "on";
    case RENOGY_CHARGE_MPPT:        return "mppt";
    case RENOGY_CHARGE_EQUALIZING:  return "equalize";
    case RENOGY_CHARGE_BOOST:       return "boost";
    case RENOGY_CHARGE_FLOATING:    return "float";
    case RENOGY_CHARGE_CURRENT_LIM: return "current-limit";
    default:                        return "?";
    }
}
