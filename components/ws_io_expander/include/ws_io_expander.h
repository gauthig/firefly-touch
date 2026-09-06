/*
 * Minimal driver for the Waveshare "IO_EXTENSION" IO expander used on the
 * ESP32-S3-Touch-LCD-7B (and 5, 4.3E, ...). It is NOT a CH422G:
 *
 *   - one 7-bit I2C address, 0x24 (the CH422G spreads its "registers" across
 *     0x23/0x24/0x38 with no register-pointer byte);
 *   - a real register-pointer model: every access is a 2-byte write
 *     { reg, value }, so it can also drive a PWM output and read an ADC.
 *
 * Register map, from Waveshare's own 7B demo (lib/io_extension):
 *   0x02  MODE    per-bit direction, 1 = output
 *   0x03  OUTPUT  push-pull output levels, bit N = EXIO N
 *   0x04  INPUT   input reads
 *   0x05  PWM     backlight PWM duty (0..255)
 *   0x06  ADC
 *
 * EXIO assignments on the 7-inch boards are the same as the CH422G ones:
 *   1 = touch reset, 2 = backlight enable, 3 = LCD reset,
 *   4 = SD card CS, 5 = USB(0) / CAN(1) mux.
 */
#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Add the expander to `bus`, set every EXIO to output, drive them all low. */
esp_err_t ws_io_expander_init(i2c_master_bus_handle_t bus);

/* Set a single EXIO pin (0..7); other pins keep their cached state. */
esp_err_t ws_io_expander_set_pin(uint8_t pin, bool level);

/* Absolute write of all eight output lines. */
esp_err_t ws_io_expander_write_io(uint8_t value);

/* Backlight PWM duty, 0..100 %. The chip has a dedicated PWM output; this is
 * a hardware dim, unlike the CH422G's on/off-only line. */
esp_err_t ws_io_expander_set_backlight_pct(uint8_t percent);

#ifdef __cplusplus
}
#endif
