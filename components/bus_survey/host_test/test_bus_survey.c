/*
 * Host tests for bus_survey (pure C).
 *   gcc -Wall -Wextra -Werror -I../include ../bus_survey.c test_bus_survey.c -o test_bus_survey
 */
#include <stdio.h>
#include <string.h>

#include "bus_survey.h"

static int g_fail;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
            g_fail++;                                                        \
        }                                                                    \
    } while (0)

#define TAB 8

static const uint8_t k_a[8] = { 0x19, 0x00, 0xC8, 0x00, 0xFF, 0x00, 0xFF, 0xFF };
static const uint8_t k_b[8] = { 0x19, 0x00, 0x64, 0x00, 0xFF, 0x00, 0xFF, 0xFF };

static void test_canonical(void)
{
    uint8_t da = 0;
    /* G6 acknowledgment: PF 0xE8 is destination-addressed, low byte is the DA. */
    CHECK(survey_canonical_dgn(0x0E8FFu, &da) == SURVEY_DGN_ACK);
    CHECK(da == 0xFFu);
    CHECK(survey_canonical_dgn(0x0EA80u, &da) == SURVEY_DGN_REQUEST);
    CHECK(da == 0x80u);
    /* PF >= 0xF0 is PDU2: the low byte is part of the group and stays. */
    CHECK(survey_canonical_dgn(0x1FEDAu, &da) == 0x1FEDAu);
    CHECK(da == 0xFFu);
    CHECK(survey_canonical_dgn(0x1FFB7u, NULL) == 0x1FFB7u);
    CHECK(survey_canonical_dgn(0x0EE00u, NULL) == SURVEY_DGN_ADDR_CLAIM);
}

static void test_new_seen_changed(void)
{
    survey_entry_t tab[TAB];
    survey_t s;
    survey_init(&s, tab, TAB, 0);

    CHECK(survey_note(&s, 1000, 0x1FEDA, 0x8F, 8, k_a) == SURVEY_NEW);
    CHECK(s.used == 1);
    CHECK(survey_note(&s, 2000, 0x1FEDA, 0x8F, 8, k_a) == SURVEY_SEEN);
    CHECK(survey_note(&s, 3500, 0x1FEDA, 0x8F, 8, k_b) == SURVEY_CHANGED);
    CHECK(s.used == 1);
    CHECK(tab[0].count == 3);
    CHECK(tab[0].changes == 1);
    CHECK(tab[0].min_dt_ms == 1000);
    CHECK(tab[0].max_dt_ms == 1500);
    CHECK(tab[0].b0 == 0x19);
    CHECK(tab[0].last[2] == 0x64);
    CHECK(s.total_frames == 3);
}

static void test_key_separation(void)
{
    survey_entry_t tab[TAB];
    survey_t s;
    survey_init(&s, tab, TAB, 0);

    uint8_t other_inst[8];
    memcpy(other_inst, k_a, 8);
    other_inst[0] = 0x1A;

    CHECK(survey_note(&s, 10, 0x1FEDA, 0x8F, 8, k_a) == SURVEY_NEW);
    CHECK(survey_note(&s, 11, 0x1FEDA, 0x8F, 8, other_inst) == SURVEY_NEW);  /* instance */
    CHECK(survey_note(&s, 12, 0x1FEDA, 0x9C, 8, k_a) == SURVEY_NEW);         /* source */
    CHECK(survey_note(&s, 13, 0x1FEDB, 0x8F, 8, k_a) == SURVEY_NEW);         /* DGN */
    CHECK(s.used == 4);

    /* PDU1 frames to different destinations are the same group. */
    CHECK(survey_note(&s, 20, 0x0E8FF, 0x8F, 8, k_a) == SURVEY_NEW);
    CHECK(survey_note(&s, 21, 0x0E880, 0x8F, 8, k_a) == SURVEY_SEEN);
    CHECK(s.used == 5);

    /* A zero-length frame has no instance byte. */
    CHECK(survey_note(&s, 30, 0x1FFFD, 0x50, 0, k_a) == SURVEY_NEW);
    CHECK(tab[s.used - 1].b0 == 0xFF);
}

static void test_full(void)
{
    survey_entry_t tab[2];
    survey_t s;
    survey_init(&s, tab, 2, 0);

    CHECK(survey_note(&s, 1, 0x1FEDA, 0x01, 8, k_a) == SURVEY_NEW);
    CHECK(survey_note(&s, 2, 0x1FEDA, 0x02, 8, k_a) == SURVEY_NEW);
    CHECK(survey_note(&s, 3, 0x1FEDA, 0x03, 8, k_a) == SURVEY_FULL);
    CHECK(s.dropped_full == 1);
    CHECK(s.used == 2);
    /* A known key still updates when the table is full. */
    CHECK(survey_note(&s, 4, 0x1FEDA, 0x01, 8, k_a) == SURVEY_SEEN);
    CHECK(tab[0].count == 2);
}

static void test_dgn_seen(void)
{
    survey_entry_t tab[TAB];
    survey_t s;
    survey_init(&s, tab, TAB, 0);

    CHECK(!survey_dgn_seen(&s, 0x0FFC0));
    survey_note(&s, 1, 0x0FFC0, 0x42, 8, k_a);
    CHECK(survey_dgn_seen(&s, 0x0FFC0));
    CHECK(!survey_dgn_seen(&s, 0x0FFC1));
    /* Seen from any source counts. */
    survey_note(&s, 2, 0x0FFC1, 0x99, 8, k_a);
    CHECK(survey_dgn_seen(&s, 0x0FFC1));
}

/* A steady 1 Hz key, then the same key at half rate after the baseline. */
static void test_deltas(void)
{
    survey_entry_t tab[TAB];
    survey_t s;
    survey_delta_t d[TAB];
    survey_init(&s, tab, TAB, 0);

    uint32_t t = 0;
    for (int i = 0; i < 60; i++) {                 /* 60 s at 1 Hz = baseline window */
        survey_note(&s, t, 0x1FFB7, 0x48, 8, k_a);
        survey_note(&s, t, 0x1FEDA, 0x8F, 8, k_b);  /* second key, stays steady */
        t += 1000;
    }
    CHECK(survey_deltas(&s, t, d, TAB, 10, 30) == 0);   /* no baseline yet */
    survey_set_baseline(&s, t);

    for (int i = 0; i < 60; i++) {                 /* next 60 s */
        if (i % 2 == 0) {
            survey_note(&s, t, 0x1FFB7, 0x48, 8, k_a);   /* slowed to half rate */
        }
        survey_note(&s, t, 0x1FEDA, 0x8F, 8, k_b);       /* unchanged */
        t += 1000;
    }
    /* A brand new key after the baseline (this is what the probe would look like). */
    survey_note(&s, t, 0x0FFC0, 0x80, 8, k_a);

    const size_t n = survey_deltas(&s, t, d, TAB, 10, 30);
    CHECK(n == 2);
    bool slowed = false, fresh = false, steady_flagged = false;
    for (size_t i = 0; i < n; i++) {
        const survey_entry_t *e = &tab[d[i].idx];
        if (e->dgn == 0x1FFB7u && !d[i].is_new && d[i].now_mhz < d[i].base_mhz) {
            slowed = true;
        }
        if (e->dgn == 0x0FFC0u && d[i].is_new) {
            fresh = true;
        }
        if (e->dgn == 0x1FEDAu) {
            steady_flagged = true;
        }
    }
    CHECK(slowed);
    CHECK(fresh);
    CHECK(!steady_flagged);
}

/* Too little data before the baseline must not read as a rate change. */
static void test_deltas_need_history(void)
{
    survey_entry_t tab[TAB];
    survey_t s;
    survey_delta_t d[TAB];
    survey_init(&s, tab, TAB, 0);

    survey_note(&s, 0, 0x1FFB7, 0x48, 8, k_a);
    survey_note(&s, 2000, 0x1FFB7, 0x48, 8, k_a);
    survey_set_baseline(&s, 5000);                 /* 5 s window, 2 frames */
    survey_note(&s, 40000, 0x1FFB7, 0x48, 8, k_a);
    CHECK(survey_deltas(&s, 70000, d, TAB, 10, 30) == 0);
}

static void test_frame_bits(void)
{
    CHECK(survey_frame_bits(0) < survey_frame_bits(8));
    /* Nominal 8-byte extended frame is 128 bits before stuffing; allow stuffing. */
    CHECK(survey_frame_bits(8) > 128);
    CHECK(survey_frame_bits(8) < 175);
    CHECK(survey_frame_bits(200) == survey_frame_bits(8));   /* DLC is clamped */
}

int main(void)
{
    test_canonical();
    test_new_seen_changed();
    test_key_separation();
    test_full();
    test_dgn_seen();
    test_deltas();
    test_deltas_need_history();
    test_frame_bits();

    if (g_fail == 0) {
        printf("bus_survey: all tests passed\n");
        return 0;
    }
    printf("bus_survey: %d FAILED\n", g_fail);
    return 1;
}
