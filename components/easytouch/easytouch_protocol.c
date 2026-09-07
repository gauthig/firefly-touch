/* See easytouch_protocol.h for the GATT map, the auth handshake, and the
 * zone-array layout with its bench-capture provenance. */
#include "easytouch_protocol.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------ TX side --- */

size_t easytouch_build_get_status(char *out, size_t out_len)
{
    if (out == NULL) {
        return 0;
    }
    /* EM/TM omitted on purpose — see the header. */
    const int n = snprintf(out, out_len, "{\"Type\":\"Get Status\",\"Zone\":0}");
    if (n < 0 || (size_t)n >= out_len) {
        if (out_len > 0) {
            out[0] = '\0';
        }
        return 0;
    }
    return (size_t)n;
}

/* Appends "<sep>\"<key>\":<val>" and advances *pos. `sep` is "" for the
 * first key and "," thereafter. Returns false on overflow. */
static bool append_int_key(char *out, size_t out_len, size_t *pos,
                           const char *sep, const char *key, int val)
{
    const int n = snprintf(out + *pos, out_len - *pos, "%s\"%s\":%d", sep, key, val);
    if (n < 0 || (size_t)n >= out_len - *pos) {
        return false;
    }
    *pos += (size_t)n;
    return true;
}

size_t easytouch_build_change(char *out, size_t out_len, const easytouch_change_t *c)
{
    if (out == NULL || c == NULL || out_len == 0) {
        return 0;
    }

    /* An empty Change is never sent. */
    const bool any = c->set_power || c->set_mode || c->set_cool_sp || c->set_heat_sp ||
                     c->set_dry_sp || c->set_auto_heat_sp || c->set_auto_cool_sp ||
                     c->set_cool_fan || c->set_heat_fan || c->set_auto_fan || c->set_fan_only;
    if (!any) {
        out[0] = '\0';
        return 0;
    }

    size_t pos = 0;
    int n = snprintf(out, out_len, "{\"Type\":\"Change\",\"Changes\":{");
    if (n < 0 || (size_t)n >= out_len) {
        out[0] = '\0';
        return 0;
    }
    pos = (size_t)n;

    bool ok = append_int_key(out, out_len, &pos, "", "zone", c->zone);

    /* Fixed key order so the output is deterministic and testable. */
    if (ok && c->set_power)        ok = append_int_key(out, out_len, &pos, ",", "power", c->power);
    if (ok && c->set_mode)         ok = append_int_key(out, out_len, &pos, ",", "mode", c->mode);
    if (ok && c->set_cool_sp)      ok = append_int_key(out, out_len, &pos, ",", "cool_sp", c->cool_sp);
    if (ok && c->set_heat_sp)      ok = append_int_key(out, out_len, &pos, ",", "heat_sp", c->heat_sp);
    if (ok && c->set_dry_sp)       ok = append_int_key(out, out_len, &pos, ",", "dry_sp", c->dry_sp);
    if (ok && c->set_auto_heat_sp) ok = append_int_key(out, out_len, &pos, ",", "autoHeat_sp", c->auto_heat_sp);
    if (ok && c->set_auto_cool_sp) ok = append_int_key(out, out_len, &pos, ",", "autoCool_sp", c->auto_cool_sp);
    if (ok && c->set_cool_fan)     ok = append_int_key(out, out_len, &pos, ",", "coolFan", c->cool_fan);
    if (ok && c->set_heat_fan)     ok = append_int_key(out, out_len, &pos, ",", "heatFan", c->heat_fan);
    if (ok && c->set_auto_fan)     ok = append_int_key(out, out_len, &pos, ",", "autoFan", c->auto_fan);
    if (ok && c->set_fan_only)     ok = append_int_key(out, out_len, &pos, ",", "fanOnly", c->fan_only);

    if (ok) {
        n = snprintf(out + pos, out_len - pos, "}}");
        if (n < 0 || (size_t)n >= out_len - pos) {
            ok = false;
        } else {
            pos += (size_t)n;
        }
    }

    if (!ok) {
        out[0] = '\0';
        return 0;
    }
    return pos;
}

/* ------------------------------------------------------------ RX side --- */

/* Bounded substring search: first occurrence of `needle` within [h, h+n). */
static bool is_json_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* Advance past ':' and any JSON whitespace, e.g. from just after a key's
 * closing quote to the first byte of its value. The real thermostat
 * pretty-prints with tabs and newlines (`"Z_sts":\t{`, `"0":\t[`), so
 * skipping only ':' and ' ' is not enough. */
static const char *skip_to_value(const char *p, const char *end)
{
    while (p < end && (*p == ':' || is_json_ws(*p))) {
        p++;
    }
    return p;
}

static const char *mem_find(const char *h, size_t n, const char *needle)
{
    const size_t m = strlen(needle);
    if (m == 0 || m > n) {
        return NULL;
    }
    for (size_t i = 0; i + m <= n; i++) {
        if (memcmp(h + i, needle, m) == 0) {
            return h + i;
        }
    }
    return NULL;
}

/*
 * Parses a comma/space-separated list of base-10 ints (optionally signed)
 * from [*p, end) into vals[0..max). Stops at ']', end, or the first
 * non-numeric, non-separator byte. Advances *p past what it consumed.
 * Returns the count parsed.
 */
static int parse_int_list(const char **p, const char *end, int *vals, int max)
{
    int count = 0;
    const char *q = *p;
    while (q < end && count < max) {
        while (q < end && (*q == ',' || *q == ' ' || *q == '\t' ||
                           *q == '\r' || *q == '\n')) {
            q++;
        }
        if (q >= end || *q == ']') {
            break;
        }
        int sign = 1;
        if (*q == '-') {
            sign = -1;
            q++;
        }
        if (q >= end || *q < '0' || *q > '9') {
            break;
        }
        long v = 0;
        while (q < end && *q >= '0' && *q <= '9') {
            v = v * 10 + (*q - '0');
            q++;
        }
        vals[count++] = (int)(sign * v);
    }
    *p = q;
    return count;
}

static void decode_zone(easytouch_zone_t *z)
{
    z->mode         = (uint8_t)z->raw[EASYTOUCH_IDX_MODE];
    z->current_mode = (uint8_t)z->raw[EASYTOUCH_IDX_CURRENT_MODE];
    z->inside_f     = (int8_t)z->raw[EASYTOUCH_IDX_INSIDE_F];
    z->cool_sp      = (uint8_t)z->raw[EASYTOUCH_IDX_COOL_SP];
    z->heat_sp      = (uint8_t)z->raw[EASYTOUCH_IDX_HEAT_SP];
    z->dry_sp       = (uint8_t)z->raw[EASYTOUCH_IDX_DRY_SP];
    z->auto_heat_sp = (uint8_t)z->raw[EASYTOUCH_IDX_AUTO_HEAT_SP];
    z->auto_cool_sp = (uint8_t)z->raw[EASYTOUCH_IDX_AUTO_COOL_SP];
    z->fan_only     = (uint8_t)z->raw[EASYTOUCH_IDX_FAN_ONLY];
    z->cool_fan     = (uint8_t)z->raw[EASYTOUCH_IDX_COOL_FAN];
    z->auto_fan     = (uint8_t)z->raw[EASYTOUCH_IDX_AUTO_FAN];
    z->heat_fan     = (uint8_t)z->raw[EASYTOUCH_IDX_HEAT_FAN];
}

bool easytouch_parse_status(const char *json, size_t len, easytouch_status_t *out)
{
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (json == NULL || len == 0) {
        return false;
    }
    const char *const end = json + len;

    /* "SN":"<digits>" — optional. */
    const char *sn = mem_find(json, len, "\"SN\"");
    if (sn) {
        const char *p = skip_to_value(sn + 4, end);
        if (p < end && *p == '"') {
            p++;
            size_t i = 0;
            while (p < end && *p != '"' && i < sizeof(out->serial) - 1) {
                out->serial[i++] = *p++;
            }
            out->serial[i] = '\0';
        }
    }

    /* "PRM":[ ... ] — logged only; not used for on/off (see header). */
    const char *prm = mem_find(json, len, "\"PRM\"");
    if (prm) {
        const char *lb = mem_find(prm, (size_t)(end - prm), "[");
        if (lb) {
            const char *p = lb + 1;
            int n = parse_int_list(&p, end, out->prm,
                                   (int)(sizeof(out->prm) / sizeof(out->prm[0])));
            if (n > 0) {
                out->prm_valid = true;
                out->prm_count = (uint8_t)n;
            }
        }
    }

    /* "Z_sts":{ "<n>":[16 ints], ... } — the required part. Anything parsed
     * above (SN, PRM) is discarded if this is missing: the contract is
     * "false ⇒ *out zeroed". */
    const char *zsts = mem_find(json, len, "\"Z_sts\"");
    const char *obj = zsts ? mem_find(zsts, (size_t)(end - zsts), "{") : NULL;
    if (!obj) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    /* Zone values are plain ints and arrays hold no braces, so the first
     * '}' at or after `obj` is the object's own close. A truncated payload
     * has no '}' — scan to `end` in that case and take whatever zones are
     * intact. */
    const char *obj_end = mem_find(obj, (size_t)(end - obj), "}");
    if (!obj_end) {
        obj_end = end;
    }

    for (const char *p = obj + 1; p < obj_end;) {
        if (*p != '"') {
            p++;
            continue;
        }
        /* "<digits>": [ */
        const char *k = p + 1;
        int zone = 0;
        int digits = 0;
        while (k < obj_end && *k >= '0' && *k <= '9') {
            zone = zone * 10 + (*k - '0');
            k++;
            digits++;
        }
        if (digits == 0 || k >= obj_end || *k != '"') {
            p++;
            continue;
        }
        k = skip_to_value(k + 1, obj_end);
        if (k >= obj_end || *k != '[') {
            p++;
            continue;
        }
        k++;

        int vals[EASYTOUCH_ZONE_ARRAY_LEN];
        const char *after = k;
        const int n = parse_int_list(&after, obj_end, vals, EASYTOUCH_ZONE_ARRAY_LEN);

        if (zone >= 0 && zone < EASYTOUCH_MAX_ZONES && n >= EASYTOUCH_ZONE_ARRAY_LEN &&
            !out->zones[zone].present) {
            easytouch_zone_t *z = &out->zones[zone];
            memcpy(z->raw, vals, sizeof(z->raw));
            z->present = true;
            decode_zone(z);
            out->present_mask |= (uint8_t)(1u << zone);
            out->zone_count++;
            if (z->current_mode != EASYTOUCH_CURRENT_IDLE) {
                out->system_active = true;
            }
        }
        p = after;
    }

    if (out->zone_count == 0) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    return true;
}

/* ------------------------------------------------------ display helpers - */

const char *easytouch_mode_str(uint8_t mode)
{
    switch (mode) {
    case EASYTOUCH_MODE_OFF:      return "Off";
    case EASYTOUCH_MODE_FAN:      return "Fan";
    case EASYTOUCH_MODE_COOL:     return "Cool";
    case EASYTOUCH_MODE_DRY:      return "Dry";
    case EASYTOUCH_MODE_AQUA_HOT: return "Aqua";
    case EASYTOUCH_MODE_HEAT:     return "Heat";  /* "Heat Pump"/"Heat Strip" is per-zone */
    case EASYTOUCH_MODE_AUTO:     return "Auto";
    default:                      return "?";
    }
}

const char *easytouch_fan_str(uint8_t fan)
{
    switch (fan) {
    case EASYTOUCH_FAN_OFF:         return "Off";
    case EASYTOUCH_FAN_LOW:         return "Low";
    case EASYTOUCH_FAN_HIGH:        return "High";
    case EASYTOUCH_FAN_CYCLED_LOW:  return "Cyc Low";
    case EASYTOUCH_FAN_CYCLED_HIGH: return "Cyc High";
    case EASYTOUCH_FAN_AUTO:        return "Auto";
    default:                        return "?";
    }
}

bool easytouch_name_matches(const char *name)
{
    static const char k_prefix[] = "EasyTouch";
    if (name == NULL) {
        return false;
    }
    return strncmp(name, k_prefix, sizeof(k_prefix) - 1) == 0;
}
