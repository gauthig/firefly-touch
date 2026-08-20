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

    printf("PASS parse_basic_info\n");
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
    test_estimate_hours();
    printf("ALL PASS\n");
    return 0;
}
