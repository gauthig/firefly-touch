/*
 * hughes_wd_protocol — Hughes Power Watchdog Gen 1 ("V1") BLE frame codec.
 *
 * Pure C, no ESP-IDF dependencies: host-testable (see
 * host_test/test_hughes_wd.c), same split as components/rvc_protocol and
 * components/jbd_bms.
 *
 * Gen 1 = the Bluetooth-only EPO models, advertising a BLE name containing
 * "PMD"/"PWS"/"PMS". (Gen 2 EPOW models advertise WD_V5/WD_E5/WD_V6/WD_E6
 * and speak a completely different "$yw@" framed protocol -- not handled
 * here.)
 *
 * ⚠️ Real-world naming: this coach's unit advertises as "APMD1CB0DE309" --
 * note the LEADING 'A'. Every public integration matches PMD/PWS/PMS as a
 * name *prefix* and would fail to detect it. Match as a SUBSTRING.
 *
 * The device streams unsolicited notifications roughly once per second once
 * you subscribe; there is no poll or init command. A 50 A unit sends one
 * complete 40-byte packet PER LINE, distinguished by the line-ID bytes at
 * the end -- not two lines in one packet. A 30 A unit only ever sends L1.
 *
 * Byte layout (offsets within the reassembled 40-byte packet):
 *
 *   | 0-2   | header 01 03 20                       |
 *   | 3-6   | voltage,   BE int32 / 10000 (V)       |
 *   | 7-10  | current,   BE int32 / 10000 (A)       |
 *   | 11-14 | power,     BE int32 / 10000 (W)       |
 *   | 15-18 | energy,    BE int32 / 10000 (kWh)     |
 *   | 19    | error code (0=OK, 1..9=E1..E9, 11/12=F1/F2) |
 *   | 20-30 | unknown / firmware-internal           |
 *   | 31-34 | frequency, BE int32 / 100 (Hz)        |
 *   | 35-36 | unknown                               |
 *   | 37-39 | line ID: 00 00 00 = L1, 01 01 01 = L2 |
 *
 * Sourced from john-k-mcdowell/My-Hughes-Power-Watchdog's docs/protocol.md,
 * which compiles the original spbrogan/tango2590/makifoxgirl ESPHome work,
 * live HCI captures against real PWD50-EPD / PWD-VM-30A hardware, and
 * decompilation of the official powerwatchdog2 Android app. Independently
 * corroborated by TechBlueprints/dbus-power-watchdog.
 *
 * TODO(bench): everything above is from those references, not yet confirmed
 * against THIS unit -- treat the field offsets the way the RV-C byte layout
 * was treated before sniffer verification. The first bench run should dump
 * raw packets (hughes_wd_client logs them) and confirm the numbers look
 * sane against the Watchdog's own display.
 *
 * NOTE ON CONTROL: Gen 1 cannot be commanded to switch power. The ASCII
 * strings (relayOn/reset/setTime/backLight) are known from the Android app,
 * but nobody has found the wire framing -- writes to 0xfff5 / 0x1003 /
 * 0x1005, with and without CRLF, are accepted at the GATT layer and ignored
 * by the device. TechBlueprints' Gen 1 handler is receive-only for the same
 * reason. This codec is therefore decode-only by design, not by omission.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Gen 1 GATT profile. */
#define HUGHES_WD_SERVICE_UUID 0xFFE0u   /* 0000ffe0-0000-1000-8000-00805f9b34fb */
#define HUGHES_WD_NOTIFY_UUID  0xFFE2u   /* device -> us, telemetry stream */
#define HUGHES_WD_WRITE_UUID   0xFFF5u   /* us -> device; see NOTE ON CONTROL */

/* One complete telemetry packet is this many bytes, delivered as two
 * 20-byte BLE notifications that must be reassembled. */
#define HUGHES_WD_PACKET_LEN 40u

/* Leading bytes that mark the start of a telemetry packet. */
#define HUGHES_WD_HDR0 0x01u
#define HUGHES_WD_HDR1 0x03u
#define HUGHES_WD_HDR2 0x20u

typedef struct {
    uint8_t line;          /* 1 or 2, from the trailing line-ID bytes */
    float   voltage_v;
    float   current_a;
    float   power_w;
    float   energy_kwh;
    float   frequency_hz;
    uint8_t error_code;    /* 0 = no error */
} hughes_wd_reading_t;

/*
 * Parses one complete 40-byte telemetry packet. Returns false if too short
 * or the 01 03 20 header is missing.
 *
 * Unlike the JBD codec there is no checksum in this protocol, so the header
 * and length are the only validation available -- callers should treat a
 * single implausible reading as suspect rather than trusting it outright.
 */
bool hughes_wd_parse_packet(const uint8_t *data, size_t len,
                            hughes_wd_reading_t *out);

/*
 * True if `name` looks like a Gen 1 Power Watchdog BLE advertisement name.
 * Matches PMD/PWS/PMS anywhere in the string, NOT just at the start -- see
 * the APMD... note in the file header.
 */
bool hughes_wd_name_matches(const char *name);

/* Human-readable form of the error code ("OK", "E1", "F2", ...). Returns a
 * pointer to static storage; never NULL. */
const char *hughes_wd_error_str(uint8_t error_code);

#ifdef __cplusplus
}
#endif
