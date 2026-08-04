/*
 * Minimal CH422G IO-expander driver (write-only, push-pull IO0..IO7).
 *
 * The CH422G does not use a register-pointer model: each "register" is its
 * own fixed I2C device address with a single data byte. Addresses per the
 * WCH datasheet / Waveshare demo:
 *   0x24  WR_SET  system parameter (bit0 IO_OE = enable IO outputs)
 *   0x23  WR_OC   open-drain outputs OC0..OC3
 *   0x38  WR_IO   push-pull outputs IO0..IO7
 *   0x26  RD_IO   input reads (unused here)
 */
#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ch422g_init(i2c_master_bus_handle_t bus);

/* Set a single EXIO pin (0..7); other pins keep their cached state. */
esp_err_t ch422g_set_pin(uint8_t pin, bool level);

/* Absolute write of all eight IO lines. */
esp_err_t ch422g_write_io(uint8_t value);

#ifdef __cplusplus
}
#endif
