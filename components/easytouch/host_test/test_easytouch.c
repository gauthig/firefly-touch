/*
 * Host-runnable unit tests for easytouch_protocol (plain asserts, no
 * framework). Build & run with any host C compiler, e.g.:
 *   gcc -Wall -Wextra -Werror -I../include ../easytouch_protocol.c test_easytouch.c -o test_easytouch && ./test_easytouch
 *
 * The status regression vectors are built from the REAL per-zone arrays,
 * PRM lists and SN captured off this coach's 355 thermostat on 2026-09-06
 * (docs/EASYTOUCH-THERMOSTAT.md §8a/§8b/§8c). The full raw JSON envelope
 * was not recorded byte-for-byte, so the surrounding fields here
 * ("Type":"Response", "REV", alert limits, ...) are representative of that
 * firmware revision's response and in the documented order; every value the
 * parser actually reads (SN, PRM, Z_sts arrays) is verbatim from the log.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "easytouch_protocol.h"

/* §8a: all three zones in Cool, fan auto, nothing actively running (except
 * zone 1's current_mode reading 2 while mode is also 2). */
static const char k_status_8a[] =
    "{\"Type\":\"Response\",\"RT\":\"Status\",\"TT\":\"EasyTouch\","
    "\"SN\":\"355003525\",\"REV\":\"1.0.7.0\",\"alertLL\":40,\"alertUL\":90,"
    "\"CI\":0,\"hA\":0,\"PRM\":[0,107,67,76],"
    "\"Z_sts\":{"
    "\"0\":[68,72,78,70,72,45,0,128,128,128,2,0,77,255,0,0],"
    "\"1\":[68,72,75,72,72,45,0,128,128,128,2,0,75,255,0,2],"
    "\"2\":[68,72,76,70,72,45,0,128,128,128,2,0,77,255,0,0]}}";

/* §8b: Front (zone 0) selected Heat Pump (mode 7), Mid Coach (zone 1)
 * selected Aqua-Hot (mode 4); both actively heating so current_mode == 4.
 * Zone 2 off. PRM shifts to the "zone 0 heating" value from §8b. */
static const char k_status_8b_heat[] =
    "{\"Type\":\"Response\",\"SN\":\"355003525\",\"PRM\":[0,107,68,79],"
    "\"Z_sts\":{"
    "\"0\":[68,72,78,70,72,45,0,128,128,128,7,0,70,255,0,4],"
    "\"1\":[68,72,75,72,72,45,0,128,128,128,4,0,71,255,0,4],"
    "\"2\":[68,72,76,70,72,45,0,128,128,128,0,0,77,255,0,0]}}";

/* §8c: Rear (zone 2) A/C on and audibly running — mode 2, current_mode 2
 * (the upstream "3 = cool running" value was never seen on this firmware). */
static const char k_status_8c_cooling[] =
    "{\"SN\":\"355003525\",\"Z_sts\":{"
    "\"2\":[68,72,74,70,72,45,0,128,128,128,2,0,75,255,0,2]}}";

/*
 * REAL 410-byte "Get Status" response, captured verbatim off this coach's
 * 355 thermostat via easytouch_client on hvac_panel (COM23, 2026-09-06).
 * ⚠️ The thermostat PRETTY-PRINTS: a TAB after every ':' and a NEWLINE
 * between entries ("Z_sts":\t{ ... "0":\t[ ... ]). The parser was rejecting
 * this until it learned to skip \t and \n, not just ':' and ' ' -- this
 * vector is the regression guard for that. All three zones in Cool.
 */
static const char k_real_status_355[] =
    "{\n"
    "\t\"Type\":\t\"Response\",\n"
    "\t\"RT\":\t\"Status\",\n"
    "\t\"TT\":\t\"EasyTouch\",\n"
    "\t\"SN\":\t\"355003525\",\n"
    "\t\"REV\":\t\"1.0.7.0\",\n"
    "\t\"alertLL\":\t40,\n"
    "\t\"alertUL\":\t90,\n"
    "\t\"CI\":\t141,\n"
    "\t\"Z_sts\":\t{\n"
    "\t\t\"0\":\t[68, 72, 75, 82, 72, 45, 0, 128, 128, 128, 2, 0, 76, 255, 0, 2],\n"
    "\t\t\"1\":\t[68, 72, 75, 80, 72, 45, 0, 128, 128, 128, 2, 0, 75, 255, 0, 0],\n"
    "\t\t\"2\":\t[68, 72, 76, 84, 72, 45, 0, 128, 128, 128, 2, 0, 77, 255, 0, 2]\n"
    "\t},\n"
    "\t\"PRM\":\t[2, 107, 66, 78],\n"
    "\t\"hA\":\t0\n"
    "}";

static void test_build_get_status(void)
{
    char buf[64];
    const size_t n = easytouch_build_get_status(buf, sizeof(buf));
    assert(n == strlen("{\"Type\":\"Get Status\",\"Zone\":0}"));
    assert(n == 30);
    assert(strcmp(buf, "{\"Type\":\"Get Status\",\"Zone\":0}") == 0);

    /* No EM/TM: the gateway has no clock (docs §3.3). */
    assert(strstr(buf, "\"EM\"") == NULL);
    assert(strstr(buf, "\"TM\"") == NULL);

    /* Buffer too small -> 0, and never a partial write left behind. */
    char tiny[8];
    assert(easytouch_build_get_status(tiny, sizeof(tiny)) == 0);
    assert(tiny[0] == '\0');
    assert(easytouch_build_get_status(NULL, 64) == 0);

    printf("PASS build_get_status\n");
}

static void test_build_change_mode(void)
{
    easytouch_change_t c = { .zone = 1, .set_mode = true, .mode = EASYTOUCH_MODE_COOL };
    char buf[128];
    const size_t n = easytouch_build_change(buf, sizeof(buf), &c);
    assert(n == strlen(buf));
    assert(strcmp(buf, "{\"Type\":\"Change\",\"Changes\":{\"zone\":1,\"mode\":2}}") == 0);

    /* Per-zone off is mode:0 with NO power key (power is system-wide). */
    easytouch_change_t off = { .zone = 2, .set_mode = true, .mode = EASYTOUCH_MODE_OFF };
    easytouch_build_change(buf, sizeof(buf), &off);
    assert(strcmp(buf, "{\"Type\":\"Change\",\"Changes\":{\"zone\":2,\"mode\":0}}") == 0);
    assert(strstr(buf, "power") == NULL);

    printf("PASS build_change_mode\n");
}

static void test_build_change_multi(void)
{
    /* mbhewitt sends power + mode + cool_sp + coolFan together; keys come
     * out in the fixed order the header documents. */
    easytouch_change_t c = {
        .zone = 0,
        .set_power = true,   .power = 1,
        .set_mode = true,    .mode = EASYTOUCH_MODE_COOL,
        .set_cool_sp = true, .cool_sp = 74,
        .set_cool_fan = true, .cool_fan = EASYTOUCH_FAN_AUTO,
    };
    char buf[160];
    easytouch_build_change(buf, sizeof(buf), &c);
    assert(strcmp(buf,
        "{\"Type\":\"Change\",\"Changes\":{\"zone\":0,\"power\":1,\"mode\":2,"
        "\"cool_sp\":74,\"coolFan\":128}}") == 0);

    /* A heat setpoint bump on its own. */
    easytouch_change_t sp = { .zone = 1, .set_heat_sp = true, .heat_sp = 68 };
    easytouch_build_change(buf, sizeof(buf), &sp);
    assert(strcmp(buf, "{\"Type\":\"Change\",\"Changes\":{\"zone\":1,\"heat_sp\":68}}") == 0);

    printf("PASS build_change_multi\n");
}

static void test_build_change_rejects(void)
{
    char buf[128];

    /* Empty Change is never sent. */
    easytouch_change_t empty = { .zone = 0 };
    assert(easytouch_build_change(buf, sizeof(buf), &empty) == 0);
    assert(buf[0] == '\0');

    /* NULL args. */
    easytouch_change_t c = { .zone = 0, .set_mode = true, .mode = 2 };
    assert(easytouch_build_change(NULL, sizeof(buf), &c) == 0);
    assert(easytouch_build_change(buf, sizeof(buf), NULL) == 0);
    assert(easytouch_build_change(buf, 0, &c) == 0);

    /* Buffer too small -> 0, no partial JSON left behind. */
    char small[24];
    assert(easytouch_build_change(small, sizeof(small), &c) == 0);
    assert(small[0] == '\0');

    printf("PASS build_change_rejects\n");
}

static void test_parse_real_status(void)
{
    easytouch_status_t st;
    assert(easytouch_parse_status(k_status_8a, strlen(k_status_8a), &st));

    assert(strcmp(st.serial, "355003525") == 0);
    assert(st.zone_count == 3);
    assert(st.present_mask == 0x07);
    assert(st.zones[0].present && st.zones[1].present && st.zones[2].present);
    assert(!st.zones[3].present);

    /* Zone 0 (Front). */
    assert(st.zones[0].mode == EASYTOUCH_MODE_COOL);
    assert(st.zones[0].current_mode == EASYTOUCH_CURRENT_IDLE);
    assert(st.zones[0].inside_f == 77);
    assert(st.zones[0].cool_sp == 78);
    assert(st.zones[0].heat_sp == 70);
    assert(st.zones[0].auto_heat_sp == 68 && st.zones[0].auto_cool_sp == 72);
    assert(st.zones[0].cool_fan == EASYTOUCH_FAN_AUTO);
    assert(strcmp(easytouch_mode_str(st.zones[0].mode), "Cool") == 0);

    /* Zone 1 (Mid Coach) — current_mode 2 while mode is also 2. */
    assert(st.zones[1].mode == EASYTOUCH_MODE_COOL);
    assert(st.zones[1].current_mode == EASYTOUCH_CURRENT_COOLING);
    assert(st.zones[1].inside_f == 75);
    /* raw idx2 == 75; §8a's summary table transcribes this as 72, but the
     * captured array is authoritative (and the doc table is corrected). */
    assert(st.zones[1].cool_sp == 75);
    assert(st.zones[1].heat_sp == 72);

    /* Zone 2 (Rear). */
    assert(st.zones[2].cool_sp == 76);
    assert(st.zones[2].current_mode == EASYTOUCH_CURRENT_IDLE);

    /* A zone actively doing something -> system_active. */
    assert(st.system_active);

    /* PRM captured verbatim, but NOT interpreted as on/off. */
    assert(st.prm_valid && st.prm_count == 4);
    assert(st.prm[0] == 0 && st.prm[1] == 107 && st.prm[2] == 67 && st.prm[3] == 76);

    /* Full raw array preserved. */
    assert(st.zones[0].raw[EASYTOUCH_IDX_MODE] == 2);
    assert(st.zones[0].raw[EASYTOUCH_IDX_INSIDE_F] == 77);
    assert(st.zones[1].raw[EASYTOUCH_IDX_CURRENT_MODE] == 2);

    printf("PASS parse_real_status\n");
}

static void test_parse_heat_modes(void)
{
    easytouch_status_t st;
    assert(easytouch_parse_status(k_status_8b_heat, strlen(k_status_8b_heat), &st));

    assert(st.zone_count == 3);

    /* mode 7 = the electric-heat slot; the protocol carries no Heat
     * Pump / Heat Strip distinction, so the generic label is "Heat". */
    assert(st.zones[0].mode == EASYTOUCH_MODE_HEAT);
    assert(strcmp(easytouch_mode_str(st.zones[0].mode), "Heat") == 0);

    /* mode 4 = Aqua-Hot (NOT the generic "heat" upstream assumed). */
    assert(st.zones[1].mode == EASYTOUCH_MODE_AQUA_HOT);
    assert(strcmp(easytouch_mode_str(st.zones[1].mode), "Aqua") == 0);

    /* current_mode == 4 while heating from EITHER source. */
    assert(st.zones[0].current_mode == EASYTOUCH_CURRENT_HEATING);
    assert(st.zones[1].current_mode == EASYTOUCH_CURRENT_HEATING);

    /* Zone 2 off. */
    assert(st.zones[2].mode == EASYTOUCH_MODE_OFF);
    assert(st.zones[2].current_mode == EASYTOUCH_CURRENT_IDLE);

    assert(st.system_active);
    assert(st.prm[2] == 68 && st.prm[3] == 79);

    printf("PASS parse_heat_modes\n");
}

static void test_parse_cooling_current(void)
{
    easytouch_status_t st;
    assert(easytouch_parse_status(k_status_8c_cooling, strlen(k_status_8c_cooling), &st));

    /* Only zone 2 present in this capture. */
    assert(st.zone_count == 1);
    assert(st.present_mask == (1u << 2));
    assert(st.zones[2].mode == EASYTOUCH_MODE_COOL);
    assert(st.zones[2].current_mode == EASYTOUCH_CURRENT_COOLING);  /* 2, not 3 */
    assert(st.system_active);

    /* No PRM in this payload. */
    assert(!st.prm_valid);

    printf("PASS parse_cooling_current\n");
}

static void test_parse_real_pretty_printed(void)
{
    easytouch_status_t st;
    assert(easytouch_parse_status(k_real_status_355, strlen(k_real_status_355), &st));

    assert(strcmp(st.serial, "355003525") == 0);
    assert(st.zone_count == 3 && st.present_mask == 0x07);

    /* All three zones Cool; zones 0 and 2 calling (current_mode 2), zone 1 idle. */
    for (uint8_t z = 0; z < 3; z++) {
        assert(st.zones[z].mode == EASYTOUCH_MODE_COOL);
    }
    assert(st.zones[0].current_mode == EASYTOUCH_CURRENT_COOLING);
    assert(st.zones[1].current_mode == EASYTOUCH_CURRENT_IDLE);
    assert(st.zones[2].current_mode == EASYTOUCH_CURRENT_COOLING);

    /* raw idx: 2 cool_sp, 3 heat_sp, 12 inside. */
    assert(st.zones[0].inside_f == 76 && st.zones[0].cool_sp == 75 && st.zones[0].heat_sp == 82);
    assert(st.zones[1].inside_f == 75 && st.zones[1].cool_sp == 75 && st.zones[1].heat_sp == 80);
    assert(st.zones[2].inside_f == 77 && st.zones[2].cool_sp == 76 && st.zones[2].heat_sp == 84);

    assert(st.system_active);

    assert(st.prm_valid && st.prm_count == 4);
    assert(st.prm[0] == 2 && st.prm[1] == 107 && st.prm[2] == 66 && st.prm[3] == 78);

    printf("PASS parse_real_pretty_printed\n");
}

static void test_parse_rejects(void)
{
    easytouch_status_t st;

    assert(!easytouch_parse_status(NULL, 10, &st));
    assert(!easytouch_parse_status(k_status_8a, strlen(k_status_8a), NULL));
    assert(!easytouch_parse_status(k_status_8a, 0, &st));

    /* No Z_sts at all -> false, and *out left zeroed. */
    const char *no_zsts = "{\"Type\":\"Response\",\"SN\":\"355003525\",\"PRM\":[0,1,2]}";
    assert(!easytouch_parse_status(no_zsts, strlen(no_zsts), &st));
    assert(st.zone_count == 0 && st.present_mask == 0);
    assert(st.serial[0] == '\0');

    /* Z_sts present but empty. */
    const char *empty_zsts = "{\"Z_sts\":{}}";
    assert(!easytouch_parse_status(empty_zsts, strlen(empty_zsts), &st));

    printf("PASS parse_rejects\n");
}

static void test_parse_truncated(void)
{
    easytouch_status_t st;

    /* Cut the §8a payload partway through zone "1"'s array (MTU-too-small
     * failure mode). Zone 0 is intact and must still be reported; zone 1's
     * short array must NOT be marked present. Callers treat "fewer zones
     * than last time" as a bad read. */
    const char *z1 = strstr(k_status_8a, "\"1\":[");
    assert(z1 != NULL);
    const size_t cut = (size_t)(z1 - k_status_8a) + 12;  /* a few ints into zone 1 */
    assert(easytouch_parse_status(k_status_8a, cut, &st));
    assert(st.zones[0].present);
    assert(!st.zones[1].present);
    assert(st.zone_count == 1);

    /* Truncated before Z_sts even begins -> false. */
    const char *zsts = strstr(k_status_8a, "\"Z_sts\"");
    const size_t before = (size_t)(zsts - k_status_8a) - 1;
    assert(!easytouch_parse_status(k_status_8a, before, &st));

    printf("PASS parse_truncated\n");
}

static void test_parse_short_zone_array(void)
{
    easytouch_status_t st;
    /* A zone that carries only 10 of the 16 ints is not "present". */
    const char *short_arr = "{\"Z_sts\":{\"0\":[1,2,3,4,5,6,7,8,9,10]}}";
    assert(!easytouch_parse_status(short_arr, strlen(short_arr), &st));
    assert(!st.zones[0].present);

    /* ...but a well-formed sibling zone still comes through. */
    const char *mixed =
        "{\"Z_sts\":{\"0\":[1,2,3,4,5,6,7,8,9,10],"
        "\"1\":[68,72,75,72,72,45,0,128,128,128,2,0,75,255,0,0]}}";
    assert(easytouch_parse_status(mixed, strlen(mixed), &st));
    assert(!st.zones[0].present);
    assert(st.zones[1].present);
    assert(st.zone_count == 1);

    printf("PASS parse_short_zone_array\n");
}

static void test_str_helpers(void)
{
    assert(strcmp(easytouch_mode_str(EASYTOUCH_MODE_OFF), "Off") == 0);
    assert(strcmp(easytouch_mode_str(EASYTOUCH_MODE_FAN), "Fan") == 0);
    assert(strcmp(easytouch_mode_str(EASYTOUCH_MODE_AUTO), "Auto") == 0);
    assert(strcmp(easytouch_mode_str(99), "?") == 0);

    assert(strcmp(easytouch_fan_str(EASYTOUCH_FAN_OFF), "Off") == 0);
    assert(strcmp(easytouch_fan_str(EASYTOUCH_FAN_AUTO), "Auto") == 0);
    assert(strcmp(easytouch_fan_str(EASYTOUCH_FAN_CYCLED_HIGH), "Cyc High") == 0);
    assert(strcmp(easytouch_fan_str(200), "?") == 0);

    printf("PASS str_helpers\n");
}

static void test_name_matches(void)
{
    assert(easytouch_name_matches("EasyTouch 355003525"));
    assert(easytouch_name_matches("EasyTouchRV"));       /* prefix, not exact */
    assert(easytouch_name_matches("EasyTouch"));
    assert(!easytouch_name_matches("xEasyTouch"));
    assert(!easytouch_name_matches("Easy"));
    assert(!easytouch_name_matches(""));
    assert(!easytouch_name_matches(NULL));

    printf("PASS name_matches\n");
}

int main(void)
{
    test_build_get_status();
    test_build_change_mode();
    test_build_change_multi();
    test_build_change_rejects();
    test_parse_real_status();
    test_parse_real_pretty_printed();
    test_parse_heat_modes();
    test_parse_cooling_current();
    test_parse_rejects();
    test_parse_truncated();
    test_parse_short_zone_array();
    test_str_helpers();
    test_name_matches();
    printf("ALL PASS\n");
    return 0;
}
