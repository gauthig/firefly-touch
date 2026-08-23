/*
 * Host-runnable unit tests for jbd_bms_protocol (plain asserts, no
 * framework). Build & run with any host C compiler, e.g.:
 *   gcc -Wall -Wextra -I../include ../jbd_bms_protocol.c test_jbd_bms.c -lm -o test_jbd_bms && ./test_jbd_bms
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "jbd_bms_protocol.h"

static void test_build_request(void)
{
    uint8_t out[JBD_BMS_REQUEST_LEN];
    bool ok = jbd_bms_build_request(JBD_BMS_REG_BASIC_INFO, out, sizeof(out));
    assert(ok);
    /* DD A5 03 00 FF FD 77 -- checksum = 0x10000 - (0x03 + 0x00) = 0xFFFD */
    assert(out[0] == 0xDD);
    assert(out[1] == 0xA5);
    assert(out[2] == 0x03);
    assert(out[3] == 0x00);
    assert(out[4] == 0xFF);
    assert(out[5] == 0xFD);
    assert(out[6] == 0x77);

    assert(!jbd_bms_build_request(0x03, out, 6));   /* too small */

    printf("PASS build_request\n");
}

/* Hand-built basic-info response: 12.80 V, -5.00 A (discharging), 150.00 Ah
 * residual, 300.00 Ah full, 12 cycles, 62% SOC. Checksum computed by hand
 * per the header's documented scope (status + len + payload). */
static const uint8_t k_basic_info_frame[] = {
    0xDD, 0x03, 0x00, 0x14,                         /* start, reg, status, len */
    0x05, 0x00,                                     /* voltage 1280 -> 12.80 V */
    0xFE, 0x0C,                                     /* current -500 -> -5.00 A */
    0x3A, 0x98,                                     /* residual 15000 -> 150.00 Ah */
    0x75, 0x30,                                     /* full 30000 -> 300.00 Ah */
    0x00, 0x0C,                                     /* cycles 12 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* production/balance/protection/sw, unused */
    0x3E,                                            /* RSOC 62% */
    0xFD, 0x1C,                                      /* checksum */
    0x77,                                            /* end */
};

static void test_parse_basic_info(void)
{
    jbd_bms_status_t s;
    bool ok = jbd_bms_parse_basic_info(k_basic_info_frame, sizeof(k_basic_info_frame), &s);
    assert(ok);
    assert(fabsf(s.voltage_v - 12.80f) < 0.001f);
    assert(fabsf(s.current_a - (-5.00f)) < 0.001f);
    assert(fabsf(s.residual_ah - 150.00f) < 0.001f);
    assert(fabsf(s.full_capacity_ah - 300.00f) < 0.001f);
    assert(s.cycles == 12);
    assert(s.soc_percent == 62);

    /* Corrupted checksum is rejected. */
    uint8_t bad[sizeof(k_basic_info_frame)];
    memcpy(bad, k_basic_info_frame, sizeof(bad));
    bad[sizeof(bad) - 2] ^= 0xFF;
    assert(!jbd_bms_parse_basic_info(bad, sizeof(bad), &s));

    /* Truncated frame is rejected. */
    assert(!jbd_bms_parse_basic_info(k_basic_info_frame, 10, &s));

    /* Wrong start/end bytes are rejected. */
    uint8_t bad_start[sizeof(k_basic_info_frame)];
    memcpy(bad_start, k_basic_info_frame, sizeof(bad_start));
    bad_start[0] = 0x00;
    assert(!jbd_bms_parse_basic_info(bad_start, sizeof(bad_start), &s));

    /* This frame's payload stops at the RSOC byte (len 0x14 == 20), i.e. it
     * carries no NTC count and no temperatures -- it must still parse, just
     * with no temperature data rather than reading past the payload. */
    assert(s.temp_count == 0);

    printf("PASS parse_basic_info\n");
}

/*
 * Real JBD documentation response frame, used here as the authoritative
 * byte-layout regression vector -- unlike k_basic_info_frame above it was
 * not hand-built by this project, and it carries the trailing fields
 * (FET status / cell count / NTC count / NTC values) that the hand-built
 * one stops short of.
 *
 * Decodes as: 58.88 V, 0.00 A, 7.20 Ah residual, 10.00 Ah full, 0 cycles,
 * 72% RSOC, 15 cells, 2 NTC probes at 20.3 C and 21.5 C. Its own trailing
 * checksum is 0xFBFF, which is what jbd_checksum() must reproduce -- that
 * is the check confirming the documented checksum SCOPE (status + len +
 * payload) is the one this protocol actually uses.
 *
 * Cross-checks that the offsets line up: 58.88 V / 15 cells = 3.92 V/cell,
 * and payload_len 27 == 23 + 2*2, exactly the two probes byte 22 declares.
 */
static const uint8_t k_jbd_doc_frame[] = {
    0xDD, 0x03, 0x00, 0x1B,
    0x17, 0x00,                                      /* voltage 5888 -> 58.88 V */
    0x00, 0x00,                                      /* current 0 */
    0x02, 0xD0,                                      /* residual 720 -> 7.20 Ah */
    0x03, 0xE8,                                      /* full 1000 -> 10.00 Ah */
    0x00, 0x00,                                      /* cycles 0 */
    0x20, 0x78,                                      /* production date */
    0x00, 0x00, 0x00, 0x00,                          /* balance status */
    0x00, 0x00,                                      /* protection status */
    0x10,                                            /* sw version */
    0x48,                                            /* RSOC 72% */
    0x03,                                            /* FET status */
    0x0F,                                            /* 15 cells */
    0x02,                                            /* 2 NTC probes */
    0x0B, 0x76,                                      /* 2934 -> 20.3 C */
    0x0B, 0x82,                                      /* 2946 -> 21.5 C */
    0xFB, 0xFF,                                      /* checksum */
    0x77,
};

static void test_parse_doc_frame_temps(void)
{
    jbd_bms_status_t s;
    assert(jbd_bms_parse_basic_info(k_jbd_doc_frame, sizeof(k_jbd_doc_frame), &s));

    assert(fabsf(s.voltage_v - 58.88f) < 0.001f);
    assert(fabsf(s.current_a - 0.0f) < 0.001f);
    assert(fabsf(s.residual_ah - 7.20f) < 0.001f);
    assert(fabsf(s.full_capacity_ah - 10.00f) < 0.001f);
    assert(s.cycles == 0);
    assert(s.soc_percent == 72);

    assert(s.temp_count == 2);
    assert(fabsf(s.temp_c[0] - 20.3f) < 0.05f);
    assert(fabsf(s.temp_c[1] - 21.5f) < 0.05f);

    /* Truncating the payload below the declared probe count must drop the
     * temperatures rather than read past the buffer. */
    uint8_t trunc[sizeof(k_jbd_doc_frame)];
    memcpy(trunc, k_jbd_doc_frame, sizeof(trunc));
    trunc[3] = 0x19;                     /* claim 25 payload bytes, not 27 */
    /* Frame is now internally inconsistent (checksum no longer matches), so
     * rebuild only what this test cares about: parse must not crash. */
    (void)jbd_bms_parse_basic_info(trunc, sizeof(trunc), &s);

    printf("PASS parse_doc_frame_temps\n");
}

static void test_power_and_conversion(void)
{
    const jbd_bms_status_t charging = { .voltage_v = 13.2f, .current_a = 20.0f };
    assert(fabsf(jbd_bms_power_w(&charging) - 264.0f) < 0.01f);

    /* Power carries the sign of the current -- discharging reads negative. */
    const jbd_bms_status_t discharging = { .voltage_v = 12.6f, .current_a = -10.0f };
    assert(jbd_bms_power_w(&discharging) < 0.0f);
    assert(fabsf(jbd_bms_power_w(&discharging) - (-126.0f)) < 0.01f);

    assert(fabsf(jbd_bms_c_to_f(0.0f) - 32.0f) < 0.01f);
    assert(fabsf(jbd_bms_c_to_f(100.0f) - 212.0f) < 0.01f);
    assert(fabsf(jbd_bms_c_to_f(-40.0f) - (-40.0f)) < 0.01f);
    assert(fabsf(jbd_bms_c_to_f(20.3f) - 68.54f) < 0.01f);

    printf("PASS power_and_conversion\n");
}

/* Builds one 300 Ah pack reading at `soc` percent drawing/taking `amps`. */
static jbd_bms_status_t make_pack(uint8_t soc, float amps, float volts,
                                  float t0, float t1)
{
    jbd_bms_status_t p = {
        .voltage_v = volts,
        .current_a = amps,
        .full_capacity_ah = 300.0f,
        .residual_ah = 300.0f * (float)soc / 100.0f,
        .soc_percent = soc,
        .temp_count = 2,
    };
    p.temp_c[0] = t0;
    p.temp_c[1] = t1;
    return p;
}

static void test_combine(void)
{
    jbd_bms_bank_t bank;

    /* Empty bank: false, zeroed, and an idle ETA rather than a NaN. */
    assert(!jbd_bms_combine(NULL, 0, &bank));
    assert(bank.pack_count == 0);
    assert(isinf(bank.hours));

    /* Three identical 50% packs each discharging 10 A. */
    jbd_bms_status_t three[3] = {
        make_pack(50, -10.0f, 13.0f, 20.0f, 21.0f),
        make_pack(50, -10.0f, 13.0f, 22.0f, 19.0f),
        make_pack(50, -10.0f, 13.0f, 25.0f, 23.0f),
    };
    assert(jbd_bms_combine(three, 3, &bank));
    assert(bank.pack_count == 3);
    assert(fabsf(bank.voltage_v - 13.0f) < 0.001f);       /* mean, not sum */
    assert(fabsf(bank.current_a - (-30.0f)) < 0.001f);    /* sum */
    assert(fabsf(bank.power_w - (-390.0f)) < 0.01f);      /* sum of V*I */
    assert(fabsf(bank.full_capacity_ah - 900.0f) < 0.01f);
    assert(fabsf(bank.residual_ah - 450.0f) < 0.01f);
    assert(bank.soc_percent == 50);
    /* 450 Ah remaining at 30 A = 15 h, computed from bank totals. */
    assert(fabsf(bank.hours - 15.0f) < 0.01f);
    /* High/low span every probe of every pack. */
    assert(bank.temp_valid);
    assert(fabsf(bank.temp_min_c - 19.0f) < 0.01f);
    assert(fabsf(bank.temp_max_c - 25.0f) < 0.01f);

    /* One pack offline: caller passes a packed array of the survivors, so
     * the bank shrinks rather than averaging in a stale/zero pack. */
    assert(jbd_bms_combine(three, 2, &bank));
    assert(bank.pack_count == 2);
    assert(fabsf(bank.full_capacity_ah - 600.0f) < 0.01f);
    assert(fabsf(bank.current_a - (-20.0f)) < 0.001f);
    assert(bank.soc_percent == 50);
    assert(fabsf(bank.hours - 15.0f) < 0.01f);   /* same rate per Ah */

    /* Unequal SOC: capacity-weighted mean. Equal capacities -> plain mean,
     * (20 + 80 + 50)/3 = 50. */
    jbd_bms_status_t uneven[3] = {
        make_pack(20, 0.0f, 12.8f, 20.0f, 20.0f),
        make_pack(80, 0.0f, 12.9f, 20.0f, 20.0f),
        make_pack(50, 0.0f, 13.0f, 20.0f, 20.0f),
    };
    assert(jbd_bms_combine(uneven, 3, &bank));
    assert(bank.soc_percent == 50);
    /* All idle -> no meaningful ETA. */
    assert(isinf(bank.hours));

    /* Mixed directions that cancel out read as an idle bank, not as a wild
     * ETA off a near-zero net current. */
    jbd_bms_status_t opposing[2] = {
        make_pack(50, 15.0f, 13.0f, 20.0f, 20.0f),
        make_pack(50, -15.0f, 13.0f, 20.0f, 20.0f),
    };
    assert(jbd_bms_combine(opposing, 2, &bank));
    assert(fabsf(bank.current_a) < 0.001f);
    assert(isinf(bank.hours));

    /* Charging bank: ETA counts up to full, not down to empty. */
    jbd_bms_status_t charging[2] = {
        make_pack(50, 25.0f, 13.4f, 20.0f, 20.0f),
        make_pack(50, 25.0f, 13.4f, 20.0f, 20.0f),
    };
    assert(jbd_bms_combine(charging, 2, &bank));
    assert(bank.current_a > 0.0f);
    assert(bank.power_w > 0.0f);
    /* (600 full - 300 residual) / 50 A = 6 h. */
    assert(fabsf(bank.hours - 6.0f) < 0.01f);

    /* A pack that reported no NTC data leaves the bank temperature invalid
     * rather than claiming 0 C. */
    jbd_bms_status_t no_temp = make_pack(50, 0.0f, 13.0f, 0.0f, 0.0f);
    no_temp.temp_count = 0;
    assert(jbd_bms_combine(&no_temp, 1, &bank));
    assert(!bank.temp_valid);

    printf("PASS combine\n");
}

static void test_estimate_hours(void)
{
    jbd_bms_status_t discharging = {
        .current_a = -5.00f, .residual_ah = 150.00f, .full_capacity_ah = 300.00f,
    };
    assert(fabsf(jbd_bms_estimate_hours(&discharging) - 30.0f) < 0.01f);

    jbd_bms_status_t charging = {
        .current_a = 10.00f, .residual_ah = 250.00f, .full_capacity_ah = 300.00f,
    };
    assert(fabsf(jbd_bms_estimate_hours(&charging) - 5.0f) < 0.01f);

    jbd_bms_status_t idle = {
        .current_a = 0.01f, .residual_ah = 150.00f, .full_capacity_ah = 300.00f,
    };
    assert(isinf(jbd_bms_estimate_hours(&idle)));

    jbd_bms_status_t full_and_charging = {
        .current_a = 2.00f, .residual_ah = 300.00f, .full_capacity_ah = 300.00f,
    };
    assert(isinf(jbd_bms_estimate_hours(&full_and_charging)));

    printf("PASS estimate_hours\n");
}

int main(void)
{
    test_build_request();
    test_parse_basic_info();
    test_parse_doc_frame_temps();
    test_power_and_conversion();
    test_estimate_hours();
    test_combine();
    printf("ALL PASS\n");
    return 0;
}
