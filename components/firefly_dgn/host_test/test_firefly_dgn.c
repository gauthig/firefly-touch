/*
 * Host tests for firefly_dgn. Golden vectors are worked out by hand from
 * docs/RVC-CUSTOM-DGNS.md, independently of the encoder.
 *   gcc -Wall -Wextra -Werror -I../include ../firefly_dgn.c test_firefly_dgn.c -lm -o test_firefly_dgn
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "firefly_dgn.h"

static int g_fail;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
            g_fail++;                                                        \
        }                                                                    \
    } while (0)

#define NEAR(a, b, tol) (fabsf((float)(a) - (float)(b)) <= (tol))

static void check_bytes(const char *what, const uint8_t *got, const uint8_t *want)
{
    if (memcmp(got, want, FDGN_LEN) != 0) {
        printf("FAIL %s: got", what);
        for (int i = 0; i < 8; i++) {
            printf(" %02X", got[i]);
        }
        printf(" want");
        for (int i = 0; i < 8; i++) {
            printf(" %02X", want[i]);
        }
        printf("\n");
        g_fail++;
    }
}

static void test_block(void)
{
    CHECK(fdgn_is_ours(0x0FFC0));
    CHECK(fdgn_is_ours(0x0FFCF));
    CHECK(!fdgn_is_ours(0x0FFBF));
    CHECK(!fdgn_is_ours(0x0FFD0));
    CHECK(!fdgn_is_ours(0x1FFC0));   /* data page 1 is a different DGN */
    CHECK(fdgn_pack_dgn(1) == 0x0FFC2 && fdgn_pack_dgn(3) == 0x0FFC4);
    CHECK(fdgn_pack_dgn(0) == 0 && fdgn_pack_dgn(4) == 0);
    CHECK(fdgn_pack_from_dgn(0x0FFC3) == 2);
    CHECK(fdgn_pack_from_dgn(0x0FFC5) == 0);   /* bank A is not a pack */
}

static void test_hvac(void)
{
    uint8_t b[8];
    /* Zone 1, present+fresh, Aqua-Hot heating, fan auto, 68 F inside, 74/70 setpoints. */
    const fdgn_hvac_status_t s = { .zone = 1, .present = true, .fresh = true, .mode = 4,
                                   .current_mode = 4, .fan = 128, .inside_f = 68,
                                   .cool_sp_f = 74, .heat_sp_f = 70 };
    fdgn_encode_hvac_status(&s, b);
    check_bytes("hvac status", b, (const uint8_t[]){ 0x01, 0x03, 0x04, 0x04, 0x80, 0x44, 0x4A, 0x46 });

    fdgn_hvac_status_t r;
    CHECK(fdgn_decode_hvac_status(b, 8, &r));
    CHECK(r.zone == 1 && r.present && r.fresh && r.mode == 4 && r.fan == 128);
    CHECK(r.inside_f == 68 && r.cool_sp_f == 74 && r.heat_sp_f == 70);

    /* Negative inside temperature survives as signed. */
    fdgn_hvac_status_t cold = s;
    cold.inside_f = -5;
    cold.fresh = false;
    fdgn_encode_hvac_status(&cold, b);
    CHECK(b[1] == 0x01 && b[5] == 0xFB);
    CHECK(fdgn_decode_hvac_status(b, 8, &r) && r.inside_f == -5 && !r.fresh);

    /* Bad zone, short frame. */
    b[0] = 4;
    CHECK(!fdgn_decode_hvac_status(b, 8, &r));
    b[0] = 1;
    CHECK(!fdgn_decode_hvac_status(b, 7, &r));

    const fdgn_hvac_extras_t x = { .zone = 2, .auto_heat_sp_f = 66, .auto_cool_sp_f = 76,
                                   .age_s = 31, .fan_only = 1, .cool_fan = 128,
                                   .heat_fan = 65, .auto_fan = 128 };
    fdgn_encode_hvac_extras(&x, b);
    check_bytes("hvac extras", b, (const uint8_t[]){ 0x02, 0x42, 0x4C, 0x1F, 0x01, 0x80, 0x41, 0x80 });
    fdgn_hvac_extras_t xr;
    CHECK(fdgn_decode_hvac_extras(b, 8, &xr) && xr.age_s == 31 && xr.heat_fan == 65);

    fdgn_hvac_extras_t old = x;   /* age saturates at 254, 255 stays n/a */
    old.age_s = 254;
    fdgn_encode_hvac_extras(&old, b);
    CHECK(b[3] == 0xFE);
    old.age_s = FDGN_U8_NA;
    fdgn_encode_hvac_extras(&old, b);
    CHECK(b[3] == 0xFF);

    /* Command: zone 0, set heat setpoint 71. */
    const fdgn_hvac_cmd_t c = { .zone = 0, .op = FDGN_HVAC_OP_HEAT_SP, .arg = 71 };
    fdgn_encode_hvac_cmd(&c, b);
    check_bytes("hvac cmd", b, (const uint8_t[]){ 0x00, 0x02, 0x47, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF });
    fdgn_hvac_cmd_t cr;
    CHECK(fdgn_decode_hvac_cmd(b, 8, &cr) && cr.op == 2 && cr.arg == 71);
    b[1] = 9;                      /* unknown op: never acted on */
    CHECK(!fdgn_decode_hvac_cmd(b, 8, &cr));
    b[1] = 2;
    b[0] = 0xFF;                   /* the probe's "not available" instance */
    CHECK(!fdgn_decode_hvac_cmd(b, 8, &cr));
}

static void test_battery(void)
{
    uint8_t b[8];
    /* Pack 2: online, temps valid, 87 %, 53.28 V, discharging 12.5 A. */
    const fdgn_batt_pack_t p = { .pack = 2, .online = true, .temps_valid = true,
                                 .soc_pct = 87, .volts = 53.28f, .amps = -12.5f };
    fdgn_encode_batt_pack(&p, b);
    check_bytes("pack", b, (const uint8_t[]){ 0x02, 0x03, 0x57, 0xD0, 0x14, 0x83, 0xFF, 0xFF });

    fdgn_batt_pack_t r;
    CHECK(fdgn_decode_batt_pack(b, 8, &r));
    CHECK(r.pack == 2 && r.online && r.temps_valid && r.soc_pct == 87);
    CHECK(NEAR(r.volts, 53.28f, 0.001f) && NEAR(r.amps, -12.5f, 0.001f));

    /* Offline pack with nothing known. */
    const fdgn_batt_pack_t off = { .pack = 3, .online = false, .temps_valid = false,
                                   .soc_pct = FDGN_U8_NA, .volts = NAN, .amps = NAN };
    fdgn_encode_batt_pack(&off, b);
    check_bytes("pack offline", b, (const uint8_t[]){ 0x03, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0xFF });
    CHECK(fdgn_decode_batt_pack(b, 8, &r) && !r.online && isnan(r.volts) && isnan(r.amps));

    /* Saturation: a huge charge current stops short of the n/a code. */
    fdgn_batt_pack_t big = p;
    big.amps = 5000.0f;
    fdgn_encode_batt_pack(&big, b);
    CHECK(b[5] == 0xFE && b[6] == 0x7F);              /* 32766 */
    big.amps = -5000.0f;
    fdgn_encode_batt_pack(&big, b);
    CHECK(b[5] == 0x00 && b[6] == 0x80);              /* -32768 */
    big.volts = -1.0f;                                 /* never negative on the wire */
    fdgn_encode_batt_pack(&big, b);
    CHECK(b[3] == 0x00 && b[4] == 0x00);

    /* Rounding to the nearest step. */
    big.amps = 0.04f;
    fdgn_encode_batt_pack(&big, b);
    CHECK(b[5] == 0x00 && b[6] == 0x00);
    big.amps = 0.06f;
    fdgn_encode_batt_pack(&big, b);
    CHECK(b[5] == 0x01 && b[6] == 0x00);

    /* Pack number out of range. */
    b[0] = 0;
    CHECK(!fdgn_decode_batt_pack(b, 8, &r));
    b[0] = 4;
    CHECK(!fdgn_decode_batt_pack(b, 8, &r));

    /* Detail: 212.4 / 300.0 Ah, 25.0 / 31.5 C. */
    const fdgn_pack_detail_t d = { .pack = 1, .residual_ah = 212.4f, .full_ah = 300.0f,
                                   .temp_min_c = 25.0f, .temp_max_c = 31.5f };
    fdgn_encode_pack_detail(&d, b);
    check_bytes("pack detail", b, (const uint8_t[]){ 0x01, 0x4C, 0x08, 0xB8, 0x0B, 0x82, 0x8F, 0xFF });
    fdgn_pack_detail_t dr;
    CHECK(fdgn_decode_pack_detail(b, 8, &dr));
    CHECK(NEAR(dr.residual_ah, 212.4f, 0.01f) && NEAR(dr.full_ah, 300.0f, 0.01f));
    CHECK(NEAR(dr.temp_min_c, 25.0f, 0.001f) && NEAR(dr.temp_max_c, 31.5f, 0.001f));

    /* Compact temperature limits and rounding. */
    fdgn_pack_detail_t t = d;
    t.temp_min_c = -40.0f;
    t.temp_max_c = 200.0f;                             /* saturates, never hits 0xFF */
    fdgn_encode_pack_detail(&t, b);
    CHECK(b[5] == 0x00 && b[6] == 0xFE);
    t.temp_min_c = -60.0f;
    t.temp_max_c = NAN;
    fdgn_encode_pack_detail(&t, b);
    CHECK(b[5] == 0x00 && b[6] == 0xFF);
    t.temp_min_c = 25.3f;                              /* 130.6 -> 131 -> 25.5 C */
    fdgn_encode_pack_detail(&t, b);
    CHECK(b[5] == 0x83);
    CHECK(fdgn_decode_pack_detail(b, 8, &dr) && NEAR(dr.temp_min_c, 25.5f, 0.001f) && isnan(dr.temp_max_c));

    /* Bank A: 3 configured, 2 online (one pack dropped), 76 %, 53.30 V, +41.7 A. */
    const fdgn_bank_a_t a = { .bank = 1, .packs_configured = 3, .packs_online = 2,
                              .soc_pct = 76, .volts = 53.30f, .amps = 41.7f };
    fdgn_encode_bank_a(&a, b);
    check_bytes("bank a", b, (const uint8_t[]){ 0x01, 0x32, 0x4C, 0xD2, 0x14, 0xA1, 0x01, 0xFF });
    fdgn_bank_a_t ar;
    CHECK(fdgn_decode_bank_a(b, 8, &ar) && ar.packs_configured == 3 && ar.packs_online == 2);
    CHECK(NEAR(ar.amps, 41.7f, 0.001f));

    /* Bank B: discharging 1234 W, 13 h 23 min left, 30.0 / 18.5 C. */
    const fdgn_bank_b_t bb = { .bank = 1, .watts = -1234.0f, .minutes_left = 803,
                               .temp_high_c = 30.0f, .temp_low_c = 18.5f,
                               .state = FDGN_BANK_DISCHARGING };
    fdgn_encode_bank_b(&bb, b);
    check_bytes("bank b", b, (const uint8_t[]){ 0x01, 0x2E, 0xFB, 0x23, 0x03, 0x8C, 0x75, 0x02 });
    fdgn_bank_b_t br;
    CHECK(fdgn_decode_bank_b(b, 8, &br) && NEAR(br.watts, -1234.0f, 0.5f) && br.minutes_left == 803);

    fdgn_bank_b_t idle = bb;   /* idle: no time remaining, power saturates both ways */
    idle.minutes_left = FDGN_U16_NA;
    idle.watts = 99999.0f;
    fdgn_encode_bank_b(&idle, b);
    CHECK(b[3] == 0xFF && b[4] == 0xFF && b[1] == 0xFE && b[2] == 0x7F);
    idle.minutes_left = 0xFFFF - 1;
    fdgn_encode_bank_b(&idle, b);
    CHECK(b[3] == 0xFE && b[4] == 0xFF);
}

static void test_shore(void)
{
    uint8_t b[8];
    /* Line B: 121.4 V, 12.3 A, 1493 W, 60.0 Hz. */
    const fdgn_shore_line_t l = { .line = 1, .volts = 121.4f, .amps = 12.3f,
                                  .watts = 1493.0f, .hz = 60.0f };
    fdgn_encode_shore_line(&l, b);
    check_bytes("shore line", b, (const uint8_t[]){ 0x01, 0xBE, 0x04, 0x7B, 0x00, 0xD5, 0x05, 0xC8 });
    fdgn_shore_line_t r;
    CHECK(fdgn_decode_shore_line(b, 8, &r));
    CHECK(NEAR(r.volts, 121.4f, 0.01f) && NEAR(r.amps, 12.3f, 0.01f) && NEAR(r.hz, 60.0f, 0.01f));

    fdgn_shore_line_t odd = l;   /* frequency clamps inside 40.0 .. 65.4 */
    odd.hz = 70.0f;
    fdgn_encode_shore_line(&odd, b);
    CHECK(b[7] == 0xFE);
    odd.hz = 30.0f;
    fdgn_encode_shore_line(&odd, b);
    CHECK(b[7] == 0x00);
    odd.hz = NAN;
    odd.watts = 100000.0f;
    fdgn_encode_shore_line(&odd, b);
    CHECK(b[7] == 0xFF && b[5] == 0xFE && b[6] == 0xFF);
    b[0] = 2;                    /* no line C */
    CHECK(!fdgn_decode_shore_line(b, 8, &r));

    /* 30 A pedestal, Watchdog error F1 (11). */
    const fdgn_shore_pedestal_t p = { .line_count = 1, .error_code = 11 };
    fdgn_encode_shore_pedestal(&p, b);
    check_bytes("pedestal", b, (const uint8_t[]){ 0x00, 0x01, 0x0B, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF });
    fdgn_shore_pedestal_t pr;
    CHECK(fdgn_decode_shore_pedestal(b, 8, &pr) && pr.line_count == 1 && pr.error_code == 11);
}

static void test_solar(void)
{
    uint8_t b[8];
    /* MPPT, 92 %, battery 53.12 V, PV 81.65 V. */
    const fdgn_solar_a_t a = { .controller = 1, .online = true, .charge_state = 2,
                               .battery_soc_pct = 92, .battery_volts = 53.12f, .pv_volts = 81.65f };
    fdgn_encode_solar_a(&a, b);
    check_bytes("solar a", b, (const uint8_t[]){ 0x01, 0x01, 0x02, 0x5C, 0xC0, 0x14, 0xE5, 0x1F });
    fdgn_solar_a_t ar;
    CHECK(fdgn_decode_solar_a(b, 8, &ar) && ar.online && NEAR(ar.pv_volts, 81.65f, 0.001f));

    /* 4.37 A, 357 W, controller 98 F, battery 77 F. */
    const fdgn_solar_b_t s = { .controller = 1, .pv_amps = 4.37f, .pv_watts = 357.0f,
                               .controller_temp_f = 98.0f, .battery_temp_f = 77.0f };
    fdgn_encode_solar_b(&s, b);
    check_bytes("solar b", b, (const uint8_t[]){ 0x01, 0xB5, 0x01, 0x65, 0x01, 0x8A, 0x75, 0xFF });
    fdgn_solar_b_t br;
    CHECK(fdgn_decode_solar_b(b, 8, &br) && NEAR(br.controller_temp_f, 98.0f, 0.001f));

    fdgn_solar_b_t cold = s;   /* -40 F floor, hot clamps below n/a */
    cold.controller_temp_f = -50.0f;
    cold.battery_temp_f = 400.0f;
    fdgn_encode_solar_b(&cold, b);
    CHECK(b[5] == 0x00 && b[6] == 0xFE);
}

static void test_valves(void)
{
    uint8_t b[8];
    const fdgn_valve_status_t s = { .valve = 1, .position = FDGN_VALVE_CLOSED, .age_s = 12,
                                    .node_fresh = true };
    fdgn_encode_valve_status(&s, b);
    check_bytes("valve status", b, (const uint8_t[]){ 0x01, 0x01, 0x0C, 0x01, 0xFF, 0xFF, 0xFF, 0xFF });
    fdgn_valve_status_t r;
    CHECK(fdgn_decode_valve_status(b, 8, &r) && r.valve == 1 && r.position == FDGN_VALVE_CLOSED);
    b[1] = 7;                    /* unknown position reads as unknown, never as open/closed */
    CHECK(fdgn_decode_valve_status(b, 8, &r) && r.position == FDGN_VALVE_UNKNOWN);

    /* Valve command: grey, open. */
    const fdgn_valve_cmd_t c = { .valve = 0, .action = FDGN_VALVE_ACT_OPEN };
    fdgn_encode_valve_cmd(&c, b);
    check_bytes("valve cmd", b, (const uint8_t[]){ 0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF });
    fdgn_valve_cmd_t cr;
    CHECK(fdgn_decode_valve_cmd(b, 8, &cr) && cr.valve == 0 && cr.action == FDGN_VALVE_ACT_OPEN);

    /* Anything not exact is rejected: a valve must never move on garbage. */
    b[1] = 2;
    CHECK(!fdgn_decode_valve_cmd(b, 8, &cr));
    b[1] = 1;
    b[0] = 0xFF;                 /* the probe's "not available" instance */
    CHECK(!fdgn_decode_valve_cmd(b, 8, &cr));
    b[0] = 0;
    CHECK(!fdgn_decode_valve_cmd(b, 7, &cr));
    CHECK(!fdgn_decode_valve_cmd(NULL, 8, &cr));
}

static void test_heartbeat(void)
{
    uint8_t b[8];
    const fdgn_heartbeat_t h = { .gateway_id = 1, .fw_major = 2, .fw_minor = 0,
                                 .uptime_min = 1500, .links = 0x7F, .health = 0 };
    fdgn_encode_heartbeat(&h, b);
    check_bytes("heartbeat", b, (const uint8_t[]){ 0x01, 0x02, 0x00, 0xDC, 0x05, 0x7F, 0x00, 0xFF });
    fdgn_heartbeat_t r;
    CHECK(fdgn_decode_heartbeat(b, 8, &r) && r.uptime_min == 1500 && r.links == 0x7F);

    fdgn_heartbeat_t up = h;     /* uptime saturates below the n/a code */
    up.uptime_min = 0xFFFF;
    fdgn_encode_heartbeat(&up, b);
    CHECK(b[3] == 0xFE && b[4] == 0xFF);
}

/* Encode -> decode keeps every value within one step. */
static void test_round_trips(void)
{
    uint8_t b[8];
    for (int i = 0; i < 2000; i++) {
        const float v = (float)i * 0.0371f;
        const fdgn_batt_pack_t p = { .pack = 1, .online = true, .soc_pct = (uint8_t)(i % 101),
                                     .volts = v, .amps = v - 30.0f };
        fdgn_batt_pack_t r;
        fdgn_encode_batt_pack(&p, b);
        CHECK(fdgn_decode_batt_pack(b, 8, &r));
        CHECK(NEAR(r.volts, p.volts, 0.0051f) && NEAR(r.amps, p.amps, 0.051f));

        const fdgn_shore_line_t l = { .line = 0, .volts = v * 2.0f, .amps = v, .watts = v * 100.0f,
                                      .hz = 50.0f + (float)(i % 100) * 0.1f };
        fdgn_shore_line_t lr;
        fdgn_encode_shore_line(&l, b);
        CHECK(fdgn_decode_shore_line(b, 8, &lr));
        CHECK(NEAR(lr.volts, l.volts, 0.051f) && NEAR(lr.hz, l.hz, 0.051f));
    }
}

int main(void)
{
    test_block();
    test_hvac();
    test_battery();
    test_shore();
    test_solar();
    test_valves();
    test_heartbeat();
    test_round_trips();

    if (g_fail == 0) {
        printf("firefly_dgn: all tests passed\n");
        return 0;
    }
    printf("firefly_dgn: %d FAILED\n", g_fail);
    return 1;
}
