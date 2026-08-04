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
    assert(data[5] == 0xFF && data[6] == 0xFF && data[7] == 0xFF);

    /* Toggle with "no change" level */
    cmd.command = RVC_DIMMER_CMD_TOGGLE;
    cmd.level   = RVC_FIELD_NA;
    rvc_encode_dc_dimmer_command_2(&cmd, data);
    assert(data[2] == 0xFF);
    assert(data[3] == 5);

    /* Ramp up / ramp down / stop codes */
    cmd.command = RVC_DIMMER_CMD_RAMP_UP;
    rvc_encode_dc_dimmer_command_2(&cmd, data);
    assert(data[3] == 17);
    cmd.command = RVC_DIMMER_CMD_RAMP_DOWN;
    rvc_encode_dc_dimmer_command_2(&cmd, data);
    assert(data[3] == 18);
    cmd.command = RVC_DIMMER_CMD_STOP;
    rvc_encode_dc_dimmer_command_2(&cmd, data);
    assert(data[3] == 4);

    /* Out-of-range level is clamped to 200 (but 0xFF passes through as N/A) */
    cmd.level = 201;
    rvc_encode_dc_dimmer_command_2(&cmd, data);
    assert(data[2] == 200);

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
    test_level_helpers();
    printf("All rvc_protocol tests passed.\n");
    return 0;
}
