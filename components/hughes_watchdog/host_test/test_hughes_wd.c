/*
 * Host-runnable unit tests for hughes_wd_protocol (plain asserts, no
 * framework). Build & run with any host C compiler, e.g.:
 *   gcc -Wall -Wextra -I../include ../hughes_wd_protocol.c test_hughes_wd.c -o test_hughes_wd && ./test_hughes_wd
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "hughes_wd_protocol.h"

/* Builds a synthetic 40-byte Gen 1 packet. No real capture from this coach's
 * unit exists yet (see the TODO(bench) in the header), so these vectors pin
 * the DOCUMENTED layout: if someone later "fixes" an offset without a real
 * capture to justify it, these fail loudly. */
static void put_be_i32(uint8_t *p, int32_t v)
{
    const uint32_t u = (uint32_t)v;
    p[0] = (uint8_t)(u >> 24);
    p[1] = (uint8_t)(u >> 16);
    p[2] = (uint8_t)(u >> 8);
    p[3] = (uint8_t)u;
}

static void build_packet(uint8_t out[40], float volts, float amps, float watts,
                         float kwh, float hz, uint8_t err, uint8_t line)
{
    memset(out, 0, 40);
    out[0] = 0x01;
    out[1] = 0x03;
    out[2] = 0x20;
    put_be_i32(&out[3],  (int32_t)(volts * 10000.0f));
    put_be_i32(&out[7],  (int32_t)(amps  * 10000.0f));
    put_be_i32(&out[11], (int32_t)(watts * 10000.0f));
    put_be_i32(&out[15], (int32_t)(kwh   * 10000.0f));
    out[19] = err;
    put_be_i32(&out[31], (int32_t)(hz * 100.0f));
    const uint8_t id = (line == 1) ? 0x00 : 0x01;
    out[37] = id;
    out[38] = id;
    out[39] = id;
}

static void test_parse_line1(void)
{
    uint8_t pkt[40];
    build_packet(pkt, 119.7f, 18.4f, 2202.5f, 143.2f, 59.98f, 0, 1);

    hughes_wd_reading_t r;
    assert(hughes_wd_parse_packet(pkt, sizeof(pkt), &r));
    assert(r.line == 1);
    assert(fabsf(r.voltage_v - 119.7f) < 0.01f);
    assert(fabsf(r.current_a - 18.4f) < 0.01f);
    assert(fabsf(r.power_w - 2202.5f) < 0.01f);
    assert(fabsf(r.energy_kwh - 143.2f) < 0.01f);
    assert(fabsf(r.frequency_hz - 59.98f) < 0.01f);
    assert(r.error_code == 0);

    printf("PASS parse_line1\n");
}

static void test_parse_line2_and_errors(void)
{
    uint8_t pkt[40];
    build_packet(pkt, 118.2f, 21.0f, 2482.2f, 98.0f, 60.01f, 7, 2);

    hughes_wd_reading_t r;
    assert(hughes_wd_parse_packet(pkt, sizeof(pkt), &r));
    assert(r.line == 2);            /* trailing 01 01 01 */
    assert(r.error_code == 7);
    assert(strcmp(hughes_wd_error_str(7), "E7 ground lost") == 0);
    assert(strcmp(hughes_wd_error_str(0), "OK") == 0);
    assert(strcmp(hughes_wd_error_str(200), "unknown") == 0);

    printf("PASS parse_line2_and_errors\n");
}

static void test_parse_rejects(void)
{
    uint8_t pkt[40];
    hughes_wd_reading_t r;
    build_packet(pkt, 120.0f, 1.0f, 120.0f, 1.0f, 60.0f, 0, 1);

    /* Short buffer -- the two-chunk reassembly must not hand us a half
     * packet and have it parse as though it were whole. */
    assert(!hughes_wd_parse_packet(pkt, 20, &r));
    assert(!hughes_wd_parse_packet(pkt, 39, &r));

    /* Wrong header. */
    uint8_t bad[40];
    memcpy(bad, pkt, sizeof(bad));
    bad[0] = 0x02;
    assert(!hughes_wd_parse_packet(bad, sizeof(bad), &r));

    memcpy(bad, pkt, sizeof(bad));
    bad[2] = 0x21;
    assert(!hughes_wd_parse_packet(bad, sizeof(bad), &r));

    assert(!hughes_wd_parse_packet(NULL, 40, &r));
    assert(!hughes_wd_parse_packet(pkt, 40, NULL));

    printf("PASS parse_rejects\n");
}

static void test_name_matching(void)
{
    /* THE case that motivated substring matching: this coach's own unit.
     * Every public integration prefix-matches "PMD" and would miss it. */
    assert(hughes_wd_name_matches("APMD1CB0DE309"));

    /* Plain documented forms still match. */
    assert(hughes_wd_name_matches("PMD1234"));
    assert(hughes_wd_name_matches("PWS0001"));
    assert(hughes_wd_name_matches("PMS9999"));

    /* Gen 2 units speak a different protocol entirely -- must NOT match, or
     * the proxy would connect and then never decode anything. */
    assert(!hughes_wd_name_matches("WD_V5_1234"));
    assert(!hughes_wd_name_matches("WD_E6_ABCD"));

    assert(!hughes_wd_name_matches("JBD-SP15S"));
    assert(!hughes_wd_name_matches(""));
    assert(!hughes_wd_name_matches(NULL));

    printf("PASS name_matching\n");
}

int main(void)
{
    test_parse_line1();
    test_parse_line2_and_errors();
    test_parse_rejects();
    test_name_matching();
    printf("ALL PASS\n");
    return 0;
}
