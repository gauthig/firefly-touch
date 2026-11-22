#include "bus_survey.h"

#include <string.h>

#define MIN_WINDOW_MS 30000u

void survey_init(survey_t *s, survey_entry_t *storage, size_t cap, uint32_t now_ms)
{
    memset(s, 0, sizeof(*s));
    s->e = storage;
    s->cap = cap;
    s->start_ms = now_ms;
    if (storage != NULL) {
        memset(storage, 0, cap * sizeof(*storage));
    }
}

uint32_t survey_canonical_dgn(uint32_t dgn17, uint8_t *da)
{
    const uint32_t pf = (dgn17 >> 8) & 0xFFu;
    if (pf < 0xF0u) {
        if (da != NULL) {
            *da = (uint8_t)(dgn17 & 0xFFu);
        }
        return dgn17 & 0x1FF00u;
    }
    if (da != NULL) {
        *da = 0xFFu;
    }
    return dgn17 & 0x1FFFFu;
}

survey_result_t survey_note(survey_t *s, uint32_t now_ms, uint32_t dgn17, uint8_t sa,
                            uint8_t dlc, const uint8_t data[8])
{
    s->total_frames++;

    const uint32_t dgn = survey_canonical_dgn(dgn17, NULL);
    const uint8_t b0 = (dlc > 0) ? data[0] : 0xFFu;
    const uint8_t n = (dlc > 8) ? 8u : dlc;

    for (size_t i = 0; i < s->used; i++) {
        survey_entry_t *e = &s->e[i];
        if (e->dgn != dgn || e->sa != sa || e->b0 != b0) {
            continue;
        }
        const uint32_t dt = now_ms - e->last_ms;
        if (dt < e->min_dt_ms) {
            e->min_dt_ms = dt;
        }
        if (dt > e->max_dt_ms) {
            e->max_dt_ms = dt;
        }
        e->count++;
        e->last_ms = now_ms;

        bool changed = (e->dlc != n) || (memcmp(e->last, data, n) != 0);
        e->dlc = n;
        memset(e->last, 0, sizeof(e->last));
        memcpy(e->last, data, n);
        if (changed) {
            if (e->changes < UINT16_MAX) {
                e->changes++;
            }
            return SURVEY_CHANGED;
        }
        return SURVEY_SEEN;
    }

    if (s->used >= s->cap) {
        s->dropped_full++;
        return SURVEY_FULL;
    }

    survey_entry_t *e = &s->e[s->used++];
    memset(e, 0, sizeof(*e));
    e->dgn = dgn;
    e->sa = sa;
    e->b0 = b0;
    e->dlc = n;
    memcpy(e->last, data, n);
    e->count = 1;
    e->first_ms = now_ms;
    e->last_ms = now_ms;
    e->min_dt_ms = UINT32_MAX;
    e->max_dt_ms = 0;
    return SURVEY_NEW;
}

bool survey_dgn_seen(const survey_t *s, uint32_t canonical_dgn)
{
    for (size_t i = 0; i < s->used; i++) {
        if (s->e[i].dgn == canonical_dgn) {
            return true;
        }
    }
    return false;
}

void survey_set_baseline(survey_t *s, uint32_t now_ms)
{
    for (size_t i = 0; i < s->used; i++) {
        s->e[i].bl_count = s->e[i].count;
    }
    s->have_baseline = true;
    s->baseline_ms = now_ms;
}

/* frames per second * 1000, from a count over a window in ms */
static uint32_t mhz(uint32_t count, uint32_t window_ms)
{
    if (window_ms == 0) {
        return 0;
    }
    return (uint32_t)(((uint64_t)count * 1000000u) / window_ms);
}

size_t survey_deltas(const survey_t *s, uint32_t now_ms, survey_delta_t *out, size_t max,
                     uint32_t min_base_count, uint32_t pct)
{
    if (!s->have_baseline || out == NULL || max == 0) {
        return 0;
    }
    const uint32_t since = now_ms - s->baseline_ms;
    size_t n = 0;

    for (size_t i = 0; i < s->used && n < max; i++) {
        const survey_entry_t *e = &s->e[i];

        if (e->first_ms > s->baseline_ms) {
            out[n++] = (survey_delta_t){ .idx = i, .base_mhz = 0,
                                         .now_mhz = mhz(e->count, since), .is_new = true };
            continue;
        }

        const uint32_t base_window = s->baseline_ms - e->first_ms;
        if (e->bl_count < min_base_count || base_window < MIN_WINDOW_MS || since < MIN_WINDOW_MS) {
            continue;
        }
        const uint32_t base = mhz(e->bl_count - 1u, base_window);   /* N frames span N-1 gaps */
        const uint32_t cur  = mhz(e->count - e->bl_count, since);
        const uint32_t diff = (cur > base) ? (cur - base) : (base - cur);
        if (base > 0 && (uint64_t)diff * 100u > (uint64_t)base * pct) {
            out[n++] = (survey_delta_t){ .idx = i, .base_mhz = base, .now_mhz = cur,
                                         .is_new = false };
        }
    }
    return n;
}

uint32_t survey_frame_bits(uint8_t dlc)
{
    if (dlc > 8) {
        dlc = 8;
    }
    /* SOF 1 + arbitration 32 (11 base ID + SRR + IDE + 18 ext ID + RTR) + control 6
     * (r1, r0, DLC) + data + CRC 15, then CRC delimiter 1 + ACK 2 + EOF 7 +
     * intermission 3. Stuffing applies to SOF..CRC; allow the typical ~1 stuff
     * bit per 5 on that span. */
    const uint32_t span = 1u + 32u + 6u + 8u * dlc + 15u;
    return span + span / 5u + 1u + 2u + 7u + 3u;
}
