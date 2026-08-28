/*
 * Host-runnable unit tests for rvc_protocol (plain asserts, no framework).
 *
 * Build & run with any host C compiler, e.g.:
 *   gcc -Wall -Wextra -I../include ../rvc_protocol.c test_rvc.c -o test_rvc && ./test_rvc
 * or MSVC (from a Developer prompt):
 *   cl /W4 /I..\include ..\rvc_protocol.c test_rvc.c /Fe:test_rvc.exe && test_rvc.exe
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "rvc_protocol.h"

static void test_id_pack_unpack(void)
{
    /* Known-good: priority 6, DC_DIMMER_COMMAND_2, SA 0x80 -> 0x19FEDB80 */
    uint32_t id = rvc_id_pack(6, RVC_DGN_DC_DIMMER_COMMAND_2, 0x80);
    assert(id == 0x19FEDB80u);

    rvc_id_t u = rvc_id_unpack(id);
    assert(u.priority == 6);
    assert(u.dgn == RVC_DGN_DC_DIMMER_COMMAND_2);
    assert(u.source_addr == 0x80);

    /* STATUS_3 from a hypothetical G6A node at SA 0x44 */
    id = rvc_id_pack(6, RVC_DGN_DC_DIMMER_STATUS_3, 0x44);
    assert(id == 0x19FEDA44u);
    u = rvc_id_unpack(id);
    assert(u.dgn == RVC_DGN_DC_DIMMER_STATUS_3);
    assert(u.source_addr == 0x44);

    /* Round-trip across the field ranges */
    for (uint8_t prio = 0; prio <= 7; prio++) {
        u = rvc_id_unpack(rvc_id_pack(prio, 0x1FFFF, 0xFF));
        assert(u.priority == prio && u.dgn == 0x1FFFFu && u.source_addr == 0xFF);
    }

    /* Out-of-range inputs are masked, not wrapped into other fields */
    u = rvc_id_unpack(rvc_id_pack(0xFF, 0xFFFFFFFF, 0x01));
    assert(u.priority == 7 && u.dgn == 0x1FFFFu && u.source_addr == 0x01);

    printf("PASS id_pack_unpack\n");
}

static void test_encode_dimmer_command(void)
{
    rvc_dimmer_command_t cmd = {
        .instance = 25,
        .group    = RVC_FIELD_NA,
        .level    = 200,
        .command  = RVC_DIMMER_CMD_SET_LEVEL,
        .duration = RVC_FIELD_NA,
    };
    uint8_t data[8];
    rvc_encode_dc_dimmer_command_2(&cmd, data);
    assert(data[0] == 25);
    assert(data[1] == 0xFF);
    assert(data[2] == 200);
    assert(data[3] == 0);
    assert(data[4] == 0xFF);
    /* Byte 5 must be 0x00 (no interlock) — 0xFF here is suspected of
     * latching G6A loads until controller reboot. Bytes 6-7 reserved. */
    assert(data[5] == 0x00 && data[6] == 0xFF && data[7] == 0xFF);

    /* Known-good ON frame per rvc-proxy: [inst FF C8 02 FF 00 FF FF] */
    cmd.command = RVC_DIMMER_CMD_ON_DELAY;
    cmd.level   = 200;
    rvc_encode_dc_dimmer_command_2(&cmd, data);
    assert(data[2] == 0xC8 && data[3] == 0x02 && data[4] == 0xFF && data[5] == 0x00);

    /* Toggle with "no change" level */
    cmd.command = RVC_DIMMER_CMD_TOGGLE;
    cmd.level   = RVC_FIELD_NA;
    rvc_encode_dc_dimmer_command_2(&cmd, data);
    assert(data[2] == 0xFF);
    assert(data[3] == 5);

    /* Ramp up / ramp down / stop codes — per the RV-C spec table
     * (cross-checked against rvc-proxy and rvc2hass): 19 = ramp up,
     * 20 = ramp down. NOT 17/18 (ramp brightness / ramp toggle), which
     * an earlier enum revision used by mistake. */
    cmd.command = RVC_DIMMER_CMD_RAMP_UP;
    rvc_encode_dc_dimmer_command_2(&cmd, data);
    assert(data[3] == 19);
    cmd.command = RVC_DIMMER_CMD_RAMP_DOWN;
    rvc_encode_dc_dimmer_command_2(&cmd, data);
    assert(data[3] == 20);
    cmd.command = RVC_DIMMER_CMD_STOP;
    rvc_encode_dc_dimmer_command_2(&cmd, data);
    assert(data[3] == 4);
    /* Lock/unlock/flash live at 33/34/49/50 in the real spec */
    assert(RVC_DIMMER_CMD_LOCK == 33 && RVC_DIMMER_CMD_UNLOCK == 34);
    assert(RVC_DIMMER_CMD_FLASH == 49 && RVC_DIMMER_CMD_FLASH_MOMENTARILY == 50);

    /* Out-of-range level is clamped to 200 (but 0xFF passes through as N/A) */
    cmd.level = 201;
    rvc_encode_dc_dimmer_command_2(&cmd, data);
    assert(data[2] == 200);

    /*
     * The LIGHT MASTER pair, byte-for-byte as the coach's factory rocker
     * sends it (captured 2026-08-28, docs/instance_map.yaml -> light_master).
     *
     * ⚠️ REGRESSION GUARD. RVC_LEVEL_RESTORE (251) sits above RVC_LEVEL_MAX,
     * and the brightness clamp used to eat it and emit 200 instead -- turning
     * "restore what each load remembered" into "set everything to 100 %".
     * That shipped once and lit loads that had been off. If this assert ever
     * fails, the master is broken again in exactly that way.
     */
    rvc_dimmer_command_t master = {
        .instance = RVC_INSTANCE_ALL,
        .group    = 0x84,
        .level    = 0,
        .command  = RVC_DIMMER_CMD_MEMORY_OFF,
        .duration = RVC_FIELD_NA,
    };
    rvc_encode_dc_dimmer_command_2(&master, data);
    /* FF 84 00 06 FF 00 FF FF */
    assert(data[0] == 0xFF && data[1] == 0x84 && data[2] == 0x00);
    assert(data[3] == 0x06 && data[4] == 0xFF && data[5] == 0x00);
    assert(data[6] == 0xFF && data[7] == 0xFF);

    master.level   = RVC_LEVEL_RESTORE;
    master.command = RVC_DIMMER_CMD_SET_LEVEL;
    rvc_encode_dc_dimmer_command_2(&master, data);
    /* FF 84 FB 00 FF 00 FF FF -- byte 2 must stay 0xFB, NOT be clamped */
    assert(data[0] == 0xFF && data[1] == 0x84);
    assert(data[2] == RVC_LEVEL_RESTORE);
    assert(data[3] == 0x00 && data[4] == 0xFF && data[5] == 0x00);

    /* A genuine out-of-range brightness must still clamp. */
    master.level = 230;
    rvc_encode_dc_dimmer_command_2(&master, data);
    assert(data[2] == RVC_LEVEL_MAX);

    printf("PASS encode_dimmer_command\n");
}

static void test_decode_dimmer_status(void)
{
    /* Instance 25 at 50 % (level 100), typical full frame */
    uint8_t frame[8] = { 25, 0xFF, 100, 0xFC, 0xFF, 0x00, 0xFC, 0xFF };
    rvc_dimmer_status_t st;
    assert(rvc_decode_dc_dimmer_status_3(frame, 8, &st));
    assert(st.instance == 25);
    assert(st.operating_level == 100);
    assert(st.load_on == true);
    assert(rvc_level_to_percent(st.operating_level) == 50);

    /* Off */
    frame[2] = 0;
    assert(rvc_decode_dc_dimmer_status_3(frame, 8, &st));
    assert(st.load_on == false);

    /* Level 0xFF = not available -> treated as not-on */
    frame[2] = 0xFF;
    assert(rvc_decode_dc_dimmer_status_3(frame, 8, &st));
    assert(st.load_on == false);
    assert(st.operating_level == RVC_LEVEL_MAX); /* clamped */

    /* Short frame still yields instance+level */
    uint8_t short_frame[3] = { 31, 0xFF, 200 };
    assert(rvc_decode_dc_dimmer_status_3(short_frame, 3, &st));
    assert(st.instance == 31 && st.operating_level == 200 && st.load_on);
    assert(st.last_command == RVC_FIELD_NA);

    /* Too short / null rejected */
    assert(!rvc_decode_dc_dimmer_status_3(short_frame, 2, &st));
    assert(!rvc_decode_dc_dimmer_status_3(NULL, 8, &st));

    printf("PASS decode_dimmer_status\n");
}

static void test_decode_tank_status(void)
{
    /* Real frames captured 2026-08-15 from this coach's SeeLevel II
     * 709-RVC via sniffer mode -- these caught the original bug (percent
     * computed as relative_level / resolution, which always truncated to
     * 0 since relative_level < resolution: resolution is the sensor's
     * total segment count, not a percent-per-count divisor). Used
     * verbatim as regression vectors so this can't silently regress. */
    uint8_t frame[8] = { 0, 0x04, 0x20, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }; /* fresh */
    rvc_tank_status_t st;
    assert(rvc_decode_tank_status(frame, 8, &st));
    assert(st.instance == 0 && st.valid == true && st.percent == 12);

    uint8_t frame_black[8] = { 1, 0x03, 0x1C, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    assert(rvc_decode_tank_status(frame_black, 8, &st));
    assert(st.instance == 1 && st.valid == true && st.percent == 10);

    uint8_t frame_gray[8] = { 2, 0x13, 0x1C, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    assert(rvc_decode_tank_status(frame_gray, 8, &st));
    assert(st.instance == 2 && st.valid == true && st.percent == 67);

    /* Full tank: relative_level == resolution -> 100% */
    frame[0] = 0; frame[1] = 32; frame[2] = 32;
    assert(rvc_decode_tank_status(frame, 8, &st));
    assert(st.percent == 100 && st.valid);

    /* Empty tank: relative_level == 0 -> 0%, still "valid" (a real reading,
     * not "not available") */
    frame[1] = 0;
    assert(rvc_decode_tank_status(frame, 8, &st));
    assert(st.percent == 0 && st.valid);

    /* relative_level == 0xFF ("not available") -> valid=false, not 0% */
    frame[1] = RVC_FIELD_NA; frame[2] = 32;
    assert(rvc_decode_tank_status(frame, 8, &st));
    assert(st.valid == false);

    /* resolution == 0 is also "not available", not a divide-by-zero */
    frame[1] = 10; frame[2] = 0;
    assert(rvc_decode_tank_status(frame, 8, &st));
    assert(st.valid == false);

    /* Out-of-spec relative_level > resolution clamps to 100%, never reports
     * over it */
    frame[1] = 250; frame[2] = 1;
    assert(rvc_decode_tank_status(frame, 8, &st));
    assert(st.valid == true && st.percent == 100);

    /* Short frame (just instance/level/resolution) still decodes */
    uint8_t short_frame[3] = { 0, 0x04, 0x20 };
    assert(rvc_decode_tank_status(short_frame, 3, &st));
    assert(st.instance == 0 && st.percent == 12 && st.valid);

    /* Too short / null rejected */
    assert(!rvc_decode_tank_status(short_frame, 2, &st));
    assert(!rvc_decode_tank_status(NULL, 8, &st));

    printf("PASS decode_tank_status\n");
}

static void test_level_helpers(void)
{
    assert(rvc_percent_to_level(0) == 0);
    assert(rvc_percent_to_level(50) == 100);
    assert(rvc_percent_to_level(100) == 200);
    assert(rvc_percent_to_level(120) == 200);
    assert(rvc_level_to_percent(0) == 0);
    assert(rvc_level_to_percent(100) == 50);
    assert(rvc_level_to_percent(200) == 100);
    assert(rvc_level_to_percent(255) == 100);
    printf("PASS level_helpers\n");
}

int main(void)
{
    test_id_pack_unpack();
    test_encode_dimmer_command();
    test_decode_dimmer_status();
    test_decode_tank_status();
    test_level_helpers();
    printf("All rvc_protocol tests passed.\n");
    return 0;
}
