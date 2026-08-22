#include "hughes_wd_protocol.h"

#include <string.h>

/* Field offsets within the 40-byte packet (see the table in the header). */
#define OFF_VOLTAGE   3u
#define OFF_CURRENT   7u
#define OFF_POWER     11u
#define OFF_ENERGY    15u
#define OFF_ERROR     19u
#define OFF_FREQUENCY 31u
#define OFF_LINE_ID   37u

/* Values are scaled fixed-point integers, not floats, on the wire. */
#define SCALE_MILLI4 10000.0f   /* volts, amps, watts, kWh */
#define SCALE_CENTI  100.0f     /* frequency */

static int32_t be_i32(const uint8_t *p)
{
    /* Assemble unsigned, then reinterpret: shifting a signed value into the
     * high bit is undefined behaviour. In practice these fields are never
     * negative (the reference implementations disagree on signed vs
     * unsigned precisely because it never matters), but decoding as signed
     * costs nothing and avoids a nonsense huge reading if one ever is. */
    const uint32_t u = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    return (int32_t)u;
}

bool hughes_wd_parse_packet(const uint8_t *data, size_t len,
                            hughes_wd_reading_t *out)
{
    if (data == NULL || out == NULL || len < HUGHES_WD_PACKET_LEN) {
        return false;
    }
    if (data[0] != HUGHES_WD_HDR0 || data[1] != HUGHES_WD_HDR1 ||
        data[2] != HUGHES_WD_HDR2) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->voltage_v    = (float)be_i32(&data[OFF_VOLTAGE])   / SCALE_MILLI4;
    out->current_a    = (float)be_i32(&data[OFF_CURRENT])   / SCALE_MILLI4;
    out->power_w      = (float)be_i32(&data[OFF_POWER])     / SCALE_MILLI4;
    out->energy_kwh   = (float)be_i32(&data[OFF_ENERGY])    / SCALE_MILLI4;
    out->error_code   = data[OFF_ERROR];
    out->frequency_hz = (float)be_i32(&data[OFF_FREQUENCY]) / SCALE_CENTI;

    /* Line ID is three repeated bytes: 00 00 00 = L1, 01 01 01 = L2. Only
     * byte 0 is examined -- a 30 A unit only ever sends L1 anyway, and
     * treating anything non-zero as L2 keeps an unexpected encoding from
     * silently vanishing. */
    out->line = (data[OFF_LINE_ID] == 0x00) ? 1 : 2;
    return true;
}

bool hughes_wd_name_matches(const char *name)
{
    if (name == NULL) {
        return false;
    }
    /* Substring, not prefix: this coach's unit advertises "APMD1CB0DE309". */
    return strstr(name, "PMD") != NULL ||
           strstr(name, "PWS") != NULL ||
           strstr(name, "PMS") != NULL;
}

const char *hughes_wd_error_str(uint8_t error_code)
{
    switch (error_code) {
    case 0:  return "OK";
    case 1:  return "E1 L1 voltage";      /* >132 V or <104 V */
    case 2:  return "E2 L2 voltage";
    case 3:  return "E3 L1 overcurrent";
    case 4:  return "E4 L2 overcurrent";
    case 5:  return "E5 L1 hot/neutral reversed";
    case 6:  return "E6 L2 hot/neutral reversed";
    case 7:  return "E7 ground lost";
    case 8:  return "E8 no neutral";
    case 9:  return "E9 surge board depleted";
    case 11: return "F1 frequency";
    case 12: return "F2 frequency";
    default: return "unknown";
    }
}
