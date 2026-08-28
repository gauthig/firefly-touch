/*
 * renogy_solar_protocol — Renogy MPPT charge-controller frame codec.
 *
 * Pure C, no ESP-IDF dependencies: host-testable (see
 * host_test/test_renogy_solar.c), same split as components/rvc_protocol,
 * components/jbd_bms and components/hughes_watchdog.
 *
 * ⚠️ This one is NOT a bespoke vendor frame format like the other two. It is
 * plain **Modbus RTU tunnelled over BLE**. The controller itself has no
 * radio; a Renogy **BT-2** module (advertising as "BT-TH-xxxxxxxx") bridges
 * BLE to the controller's RS485 port and passes Modbus through untouched.
 *
 * ⚠️ The BT-2 PADS its advertised name with trailing spaces -- this coach's
 * sends "BT-TH-B00E7B91    " while the label says "BT-TH-B00E7B91". Compare
 * with renogy_name_equals(), never strcmp. See its comment below.
 * So: standard function-3 reads, standard CRC-16/MODBUS, and the register
 * map below is the controller's, not the radio's.
 *
 * Transaction shape, which is why this codec looks different from the
 * Watchdog's: the device is SILENT until asked. We write a read request to
 * 0xFFD1 and the answer arrives as notifications on 0xFFF1 — request /
 * response, not a stream. That is also what makes reassembly easy: exactly
 * one response is outstanding at a time, so bytes are simply appended until
 * the expected length is reached and then CRC-checked, with none of the
 * mid-stream header resync hughes_wd_protocol has to do.
 *
 *   request:  [id][0x03][reg_hi][reg_lo][words_hi][words_lo][crc_lo][crc_hi]
 *   response: [id][0x03][byte_count][data ...][crc_lo][crc_hi]
 *
 * CRC-16/MODBUS (poly 0xA001, init 0xFFFF), appended LOW BYTE FIRST. Pinned
 * by a regression vector in the host test: FF 03 01 00 00 22 -> D1 F1, which
 * is a request frame logged by the reference implementation itself.
 *
 * Register map for the "charging info" block at 0x0100. Offsets are into the
 * whole response frame, i.e. past the 3-byte [id][func][count] header:
 *
 *   | offset | register | field                  | scale    |
 *   |--------|----------|------------------------|----------|
 *   | 3-4    | 0x0100   | battery SOC            | %        |
 *   | 5-6    | 0x0101   | battery voltage        | x0.1 V   |
 *   | 7-8    | 0x0102   | battery charge current | x0.01 A  |
 *   | 9      | 0x0103hi | controller temperature | °C       |
 *   | 10     | 0x0103lo | battery temperature    | °C       |
 *   | 11-12  | 0x0104   | load voltage           | x0.1 V   |
 *   | 13-14  | 0x0105   | load current           | x0.01 A  |
 *   | 15-16  | 0x0106   | load power             | W        |
 *   | 17-18  | 0x0107   | PV (solar) voltage     | x0.1 V   |
 *   | 19-20  | 0x0108   | PV (solar) current     | x0.01 A  |
 *   | 21-22  | 0x0109   | PV (solar) power       | W        |
 *   | 67     | 0x0120hi | load on/off, bit 7     | flag     |
 *   | 68     | 0x0120lo | charging state         | enum     |
 *
 * ⚠️ **Temperature is NOT two's complement.** Bit 7 is a sign FLAG and bits
 * 0-6 are the magnitude, so 0x8F is -15 °C, not -113 °C. Decoding it as a
 * plain int8_t looks correct for every reading above freezing and only goes
 * wrong below it — which is exactly when a battery temperature matters.
 *
 * ⚠️ The charging state at offset 68 is the reason this reads 34 words
 * rather than the 10 that would cover volts/amps/watts/temperature. It is
 * what separates "0 W because it is dark" from "0 W because the battery is
 * already full", which a bare wattage cannot express. 34 words is also
 * byte-for-byte the request the reference implementation issues, so it is
 * the version with real-world mileage on it.
 *
 * Sourced from cyrils/renogy-bt, the de-facto reference for Renogy BLE
 * (BaseClient.py for the request builder and UUIDs, RoverClient.py for the
 * register map, Utils.py for the CRC table and the temperature sign flag).
 *
 * **BYTE LAYOUT VERIFIED ON THIS COACH'S CONTROLLER, 2026-08-26.** A real
 * 73-byte response captured from the basement proxy is committed as the
 * regression vector `k_real_response` in the host test, and it decodes
 * self-consistently across every field at once: byte 2 declares 0x44 = 68
 * data bytes for the 34 words requested, the frame's own trailing CRC
 * reproduces under renogy_crc16(), and the values agree with each other --
 * 14.6 V x 5.05 A = 73.7 W against a reported PV power of 74 W, with the
 * charge state reading "boost" exactly as a 14.6 V bulk charge should.
 * These offsets are no longer inference; don't renumber them without a
 * contradicting capture.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * GATT profile. Note the write and notify characteristics live in DIFFERENT
 * services — 0xFFD1 in 0xFFD0, 0xFFF1 in 0xFFF0. Both BT-1 and BT-2 modules
 * use the same profile.
 */
#define RENOGY_WRITE_SERVICE_UUID  0xFFD0u
#define RENOGY_WRITE_CHAR_UUID     0xFFD1u
#define RENOGY_NOTIFY_SERVICE_UUID 0xFFF0u
#define RENOGY_NOTIFY_CHAR_UUID    0xFFF1u

/* Modbus function codes we care about. */
#define RENOGY_FUNC_READ     0x03u
#define RENOGY_FUNC_READ_ERR 0x83u   /* exception reply: function | 0x80 */

/* The charging-info block: SOC through charging state. */
#define RENOGY_REG_CHARGING_INFO 0x0100u
#define RENOGY_CHARGING_WORDS    34u

/* Frame overhead: [id][func][byte_count] ... [crc_lo][crc_hi]. */
#define RENOGY_RESP_OVERHEAD 5u
/* A complete charging-info response: 73 bytes. */
#define RENOGY_CHARGING_RESP_LEN (RENOGY_CHARGING_WORDS * 2u + RENOGY_RESP_OVERHEAD)
/* Every request is this long. */
#define RENOGY_REQUEST_LEN 8u

/*
 * The Modbus address the controller answers on, behind the BT-2's RS485
 * bridge. 255 is the broadcast address and is what a stand-alone controller
 * responds to; daisy-chained and Communication-Hub wiring use the others.
 * Tried in this order when the configured one gets no reply — the response
 * echoes the controller's REAL address in byte 0, so a wrong guess is
 * self-correcting and visible in the log rather than looking like a dead
 * link.
 */
#define RENOGY_DEVICE_ID_BROADCAST 255u
extern const uint8_t k_renogy_device_id_candidates[];
extern const size_t  k_renogy_device_id_candidate_count;

/* Charging state, register 0x0120 low byte. */
typedef enum {
    RENOGY_CHARGE_DEACTIVATED = 0,
    RENOGY_CHARGE_ACTIVATED   = 1,
    RENOGY_CHARGE_MPPT        = 2,
    RENOGY_CHARGE_EQUALIZING  = 3,
    RENOGY_CHARGE_BOOST       = 4,
    RENOGY_CHARGE_FLOATING    = 5,
    RENOGY_CHARGE_CURRENT_LIM = 6,
} renogy_charge_state_t;

typedef struct {
    uint8_t device_id;          /* as echoed by the controller, not as asked */

    uint8_t battery_soc;        /* % */
    float   battery_voltage_v;
    float   battery_current_a;  /* into the battery */

    float   controller_temp_c;
    float   battery_temp_c;

    float    load_voltage_v;
    float    load_current_a;
    uint16_t load_power_w;
    bool     load_on;

    float    pv_voltage_v;
    float    pv_current_a;
    uint16_t pv_power_w;

    uint8_t  charge_state;      /* renogy_charge_state_t */
} renogy_solar_status_t;

/* CRC-16/MODBUS over `len` bytes. Returned host-order; callers append the
 * LOW byte first. */
uint16_t renogy_crc16(const uint8_t *data, size_t len);

/*
 * Builds a function-3 read request into `out`, which must have room for
 * RENOGY_REQUEST_LEN bytes. Returns the number of bytes written, or 0 if
 * `out` is NULL.
 */
size_t renogy_build_read_request(uint8_t device_id, uint16_t reg, uint16_t words,
                                 uint8_t *out);

/*
 * How many bytes a reply to a `words`-long read will be, so the client knows
 * when it has reassembled a whole one.
 */
size_t renogy_expected_response_len(uint16_t words);

/*
 * Validates and parses a complete charging-info response.
 *
 * Returns false if the frame is the wrong length, is a Modbus exception
 * reply (function 0x83), carries the wrong byte count, or fails its CRC.
 * Unlike the Watchdog's format there IS a checksum here, so a corrupt frame
 * can be rejected outright rather than merely looking implausible.
 */
bool renogy_parse_charging_info(const uint8_t *data, size_t len,
                                renogy_solar_status_t *out);

/*
 * Decodes one Renogy temperature byte to °C. Bit 7 is a sign flag, bits 0-6
 * the magnitude — see the warning in the file header.
 */
float renogy_decode_temp_c(uint8_t raw);

/* °C -> °F. This project reports temperatures in °F everywhere it shows
 * them, so the conversion lives in the codec next to the decode rather than
 * being repeated at each call site. */
float renogy_c_to_f(float celsius);

/*
 * True if `name` is a Renogy BLE module advertisement name. Matches the
 * known module prefixes so a controller can be found without knowing its
 * MAC.
 */
bool renogy_name_matches(const char *name);

/*
 * Compares an advertised name against a configured one, IGNORING TRAILING
 * WHITESPACE on either side.
 *
 * ⚠️ Not a plain strcmp, and the reason is real hardware: this coach's BT-2
 * advertises its name padded to a fixed field width --
 * "BT-TH-B00E7B91    ", with four trailing spaces -- while the label on the
 * device, the Renogy app and therefore anything a human types into Kconfig
 * all say "BT-TH-B00E7B91". A strcmp between the two never matches, and the
 * symptom is a scan that runs forever and finds nothing, with the device
 * sitting right there at -71 dBm.
 */
bool renogy_name_equals(const char *adv_name, const char *want);

/* Human-readable charging state ("mppt", "float", ...). Static storage,
 * never NULL. */
const char *renogy_charge_state_str(uint8_t state);

#ifdef __cplusplus
}
#endif
