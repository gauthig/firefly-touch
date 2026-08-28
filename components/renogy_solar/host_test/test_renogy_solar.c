/*
 * Host-runnable unit tests for renogy_solar_protocol (plain asserts, no
 * framework). Build & run with any host C compiler, e.g.:
 *   gcc -Wall -Wextra -I../include ../renogy_solar_protocol.c test_renogy_solar.c -lm -o test_renogy_solar && ./test_renogy_solar
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "renogy_solar_protocol.h"

/*
 * REAL regression vector, not a synthetic one: this is the request frame the
 * reference implementation (cyrils/renogy-bt) logs for its own charging-info
 * read --
 *
 *   create_read_request 256 => [255, 3, 1, 0, 0, 34, 209, 241]
 *
 * -- so it pins the whole request convention at once: broadcast device id,
 * function 3, register 0x0100 big-endian, 34 words, and a CRC-16/MODBUS
 * appended LOW BYTE FIRST (0xF1D1 -> D1 F1). If someone "tidies" the CRC
 * byte order, this is what fails.
 */
static const uint8_t k_doc_request[RENOGY_REQUEST_LEN] = {
    0xFF, 0x03, 0x01, 0x00, 0x00, 0x22, 0xD1, 0xF1,
};

/*
 * REAL 73-byte response captured from this coach's controller through its
 * BT-2, on the basement proxy, 2026-08-26. Logged verbatim by
 * renogy_solar_client's raw-frame dump.
 *
 * This is what upgrades the register map from "documented by a third party"
 * to "verified on the actual hardware", the same role k_jbd_doc_frame plays
 * for the battery codec. Its own trailing CRC reproduces under
 * renogy_crc16(), so it pins the checksum convention as well as the offsets.
 */
static const uint8_t k_real_response[RENOGY_CHARGING_RESP_LEN] = {
    0xff, 0x03, 0x44, 0x00, 0x64, 0x00, 0x92, 0x01, 0xf9, 0x30, 0x29, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x01, 0x7b, 0x00, 0x4a, 0x00,
    0x00, 0x00, 0x87, 0x00, 0x92, 0x03, 0xef, 0x00, 0x00, 0x00, 0x91, 0x00,
    0x00, 0x00, 0x30, 0x00, 0x00, 0x02, 0x9a, 0x00, 0x00, 0x07, 0x50, 0x00,
    0x00, 0x00, 0x9a, 0x00, 0x00, 0xde, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x0c, 0x0f, 0xa7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0xb4,
    0x48,
};

static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

/* Builds a well-formed charging-info response with a correct CRC. */
static void build_response(uint8_t out[RENOGY_CHARGING_RESP_LEN], uint8_t device_id)
{
    memset(out, 0, RENOGY_CHARGING_RESP_LEN);
    out[0] = device_id;
    out[1] = RENOGY_FUNC_READ;
    out[2] = (uint8_t)(RENOGY_CHARGING_WORDS * 2u);

    put_be16(&out[3], 87);        /* SOC 87 %                    */
    put_be16(&out[5], 129);       /* battery 12.9 V              */
    put_be16(&out[7], 258);       /* battery current 2.58 A      */
    out[9]  = 33;                 /* controller 33 °C            */
    out[10] = 25;                 /* battery 25 °C               */
    put_be16(&out[11], 0);        /* load 0.0 V                  */
    put_be16(&out[13], 0);        /* load 0.00 A                 */
    put_be16(&out[15], 0);        /* load 0 W                    */
    put_be16(&out[17], 171);      /* PV 17.1 V                   */
    put_be16(&out[19], 204);      /* PV 2.04 A                   */
    put_be16(&out[21], 35);       /* PV 35 W                     */
    out[67] = 0x00;               /* load off                    */
    out[68] = RENOGY_CHARGE_MPPT;

    const uint16_t crc = renogy_crc16(out, RENOGY_CHARGING_RESP_LEN - 2u);
    out[RENOGY_CHARGING_RESP_LEN - 2u] = (uint8_t)(crc & 0xFFu);
    out[RENOGY_CHARGING_RESP_LEN - 1u] = (uint8_t)(crc >> 8);
}

static void test_request_matches_reference(void)
{
    uint8_t req[RENOGY_REQUEST_LEN];
    const size_t n = renogy_build_read_request(RENOGY_DEVICE_ID_BROADCAST,
                                               RENOGY_REG_CHARGING_INFO,
                                               RENOGY_CHARGING_WORDS, req);
    assert(n == RENOGY_REQUEST_LEN);
    assert(memcmp(req, k_doc_request, sizeof(req)) == 0);

    /* The documented CRC reproduces exactly, which is what confirms the
     * polynomial, the init value and the scope (everything before the CRC). */
    assert(renogy_crc16(k_doc_request, 6) == 0xF1D1u);

    assert(renogy_build_read_request(1, 0, 1, NULL) == 0);

    printf("PASS request_matches_reference\n");
}

static void test_expected_length(void)
{
    assert(renogy_expected_response_len(RENOGY_CHARGING_WORDS) == RENOGY_CHARGING_RESP_LEN);
    assert(RENOGY_CHARGING_RESP_LEN == 73u);
    assert(renogy_expected_response_len(1) == 7u);

    printf("PASS expected_length\n");
}

static void test_parse_charging_info(void)
{
    uint8_t resp[RENOGY_CHARGING_RESP_LEN];
    build_response(resp, 97);

    renogy_solar_status_t st;
    assert(renogy_parse_charging_info(resp, sizeof(resp), &st));

    /* Byte 0 is the controller's real address even though 255 was asked for
     * -- this is what the client's device-id probe keys off. */
    assert(st.device_id == 97);

    assert(st.battery_soc == 87);
    assert(fabsf(st.battery_voltage_v - 12.9f) < 0.01f);
    assert(fabsf(st.battery_current_a - 2.58f) < 0.01f);

    assert(fabsf(st.controller_temp_c - 33.0f) < 0.01f);
    assert(fabsf(st.battery_temp_c - 25.0f) < 0.01f);

    assert(fabsf(st.pv_voltage_v - 17.1f) < 0.01f);
    assert(fabsf(st.pv_current_a - 2.04f) < 0.01f);
    assert(st.pv_power_w == 35);

    assert(st.load_power_w == 0);
    assert(!st.load_on);
    assert(st.charge_state == RENOGY_CHARGE_MPPT);

    printf("PASS parse_charging_info\n");
}

static void test_parse_real_capture(void)
{
    renogy_solar_status_t st;
    /* Parses at all, which already means the declared byte count and the
     * device's OWN checksum both check out against this decoder. */
    assert(renogy_parse_charging_info(k_real_response, sizeof(k_real_response), &st));

    assert(st.device_id == 255);          /* stand-alone: answers on broadcast */
    assert(st.battery_soc == 100);
    assert(fabsf(st.battery_voltage_v - 14.6f) < 0.01f);
    assert(fabsf(st.battery_current_a - 5.05f) < 0.01f);

    assert(fabsf(st.pv_voltage_v - 19.2f) < 0.01f);
    assert(fabsf(st.pv_current_a - 3.79f) < 0.01f);
    assert(st.pv_power_w == 74);

    /* 48 °C / 41 °C -> 118.4 °F / 105.8 °F. Hot, but this was a sunny day
     * with the controller in a closed basement bay. */
    assert(fabsf(st.controller_temp_c - 48.0f) < 0.01f);
    assert(fabsf(st.battery_temp_c - 41.0f) < 0.01f);
    assert(fabsf(renogy_c_to_f(st.controller_temp_c) - 118.4f) < 0.01f);
    assert(fabsf(renogy_c_to_f(st.battery_temp_c) - 105.8f) < 0.01f);

    assert(st.charge_state == RENOGY_CHARGE_BOOST);

    /*
     * The cross-check that makes the whole layout hang together rather than
     * being six independently plausible numbers: reported PV power must
     * agree with battery volts x amps, since that is where the energy goes.
     * 14.6 x 5.05 = 73.7 W against a reported 74 W.
     */
    const float derived_w = st.battery_voltage_v * st.battery_current_a;
    assert(fabsf(derived_w - (float)st.pv_power_w) < 1.0f);

    printf("PASS parse_real_capture\n");
}

static void test_parse_rejects(void)
{
    uint8_t resp[RENOGY_CHARGING_RESP_LEN];

    /* Truncated: a half-reassembled response must never parse. */
    build_response(resp, 255);
    assert(!renogy_parse_charging_info(resp, sizeof(resp) - 1u, NULL));
    renogy_solar_status_t st;
    assert(!renogy_parse_charging_info(resp, sizeof(resp) - 1u, &st));
    assert(!renogy_parse_charging_info(NULL, sizeof(resp), &st));
    assert(!renogy_parse_charging_info(resp, sizeof(resp), NULL));

    /* One flipped payload bit must fail the CRC. This is the check the
     * Watchdog's format cannot offer, so it is worth asserting it works. */
    build_response(resp, 255);
    resp[21] ^= 0x01u;
    assert(!renogy_parse_charging_info(resp, sizeof(resp), &st));

    /* Modbus exception reply (function | 0x80). */
    build_response(resp, 255);
    resp[1] = RENOGY_FUNC_READ_ERR;
    assert(!renogy_parse_charging_info(resp, sizeof(resp), &st));

    /* Right length, wrong declared byte count. */
    build_response(resp, 255);
    resp[2] = 20u;
    assert(!renogy_parse_charging_info(resp, sizeof(resp), &st));

    printf("PASS parse_rejects\n");
}

static void test_temperature(void)
{
    /* Positive readings: plain magnitude. */
    assert(fabsf(renogy_decode_temp_c(0) - 0.0f) < 0.01f);
    assert(fabsf(renogy_decode_temp_c(25) - 25.0f) < 0.01f);
    assert(fabsf(renogy_decode_temp_c(127) - 127.0f) < 0.01f);

    /*
     * Bit 7 is a SIGN FLAG, not two's complement. 0x8F is -15 °C; read as an
     * int8_t it would be -113 °C. This assert is the whole reason the decode
     * is its own function -- a below-freezing battery temperature is exactly
     * the reading that matters, and it is the only one that exposes the bug.
     */
    assert(fabsf(renogy_decode_temp_c(0x8F) - (-15.0f)) < 0.01f);
    assert(fabsf(renogy_decode_temp_c(0x81) - (-1.0f)) < 0.01f);
    assert(fabsf(renogy_decode_temp_c(0x80) - 0.0f) < 0.01f);

    /* °F is what gets stored and broadcast; the tenths are load-bearing,
     * since 33 °C is 91.4 °F and rounding to whole degrees would lose it. */
    assert(fabsf(renogy_c_to_f(0.0f) - 32.0f) < 0.01f);
    assert(fabsf(renogy_c_to_f(25.0f) - 77.0f) < 0.01f);
    assert(fabsf(renogy_c_to_f(33.0f) - 91.4f) < 0.01f);
    assert(fabsf(renogy_c_to_f(-15.0f) - 5.0f) < 0.01f);

    printf("PASS temperature\n");
}

static void test_name_matching(void)
{
    /* The coach's actual module. */
    assert(renogy_name_matches("BT-TH-B00E7B91"));
    assert(renogy_name_matches("BT-TH-161EXXXX"));
    assert(renogy_name_matches("BTRIC134000000"));
    assert(renogy_name_matches("RNGRBP12345678"));

    /* Prefix, not substring -- unlike the Watchdog's matcher, which has to
     * be a substring test for its leading-'A' name. */
    assert(!renogy_name_matches("XBT-TH-B00E7B91"));

    /* The proxy's other BLE peers must not match, or it would connect to a
     * battery pack and then fail to decode anything from it. */
    assert(!renogy_name_matches("APMD1CB0DE309"));
    assert(!renogy_name_matches("JBD-SP15S"));
    assert(!renogy_name_matches(""));
    assert(!renogy_name_matches(NULL));

    printf("PASS name_matching\n");
}

static void test_name_equals_ignores_padding(void)
{
    /*
     * REAL observed advertisement from this coach's BT-2, captured on the
     * proxy: the module pads its name to a fixed field width, so what comes
     * over the air is "BT-TH-B00E7B91    " while the device label -- and
     * therefore CONFIG_FIREFLY_SOLAR_NAME -- says "BT-TH-B00E7B91".
     *
     * A plain strcmp between those two fails, and the only symptom is a scan
     * that never matches anything. This is the regression guard for it.
     */
    assert(renogy_name_equals("BT-TH-B00E7B91    ", "BT-TH-B00E7B91"));
    assert(renogy_name_equals("BT-TH-B00E7B91", "BT-TH-B00E7B91    "));
    assert(renogy_name_equals("BT-TH-B00E7B91", "BT-TH-B00E7B91"));
    assert(renogy_name_equals("BT-TH-B00E7B91\t\r\n", "BT-TH-B00E7B91"));

    /* Padding is not a wildcard: a different module must still not match. */
    assert(!renogy_name_equals("BT-TH-B00E7B92    ", "BT-TH-B00E7B91"));
    assert(!renogy_name_equals("BT-TH-B00E7B9    ", "BT-TH-B00E7B91"));
    /* Leading whitespace is NOT trimmed -- a leading character can be real,
     * as the Watchdog's "APMD..." shows. */
    assert(!renogy_name_equals(" BT-TH-B00E7B91", "BT-TH-B00E7B91"));
    assert(!renogy_name_equals(NULL, "BT-TH-B00E7B91"));
    assert(!renogy_name_equals("BT-TH-B00E7B91", NULL));

    /* The prefix matcher must cope with the padded form too. */
    assert(renogy_name_matches("BT-TH-B00E7B91    "));

    printf("PASS name_equals_ignores_padding\n");
}

static void test_device_id_candidates(void)
{
    assert(k_renogy_device_id_candidate_count > 0);
    /* Broadcast is tried first: it is what a stand-alone controller answers
     * on, which is this coach's wiring. */
    assert(k_renogy_device_id_candidates[0] == RENOGY_DEVICE_ID_BROADCAST);

    printf("PASS device_id_candidates\n");
}

static void test_charge_state_str(void)
{
    assert(strcmp(renogy_charge_state_str(RENOGY_CHARGE_MPPT), "mppt") == 0);
    assert(strcmp(renogy_charge_state_str(RENOGY_CHARGE_FLOATING), "float") == 0);
    assert(strcmp(renogy_charge_state_str(200), "?") == 0);

    printf("PASS charge_state_str\n");
}

int main(void)
{
    test_request_matches_reference();
    test_expected_length();
    test_parse_charging_info();
    test_parse_real_capture();
    test_parse_rejects();
    test_temperature();
    test_name_matching();
    test_name_equals_ignores_padding();
    test_device_id_candidates();
    test_charge_state_str();
    printf("ALL PASS\n");
    return 0;
}
