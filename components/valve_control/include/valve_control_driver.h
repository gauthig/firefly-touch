/*
 * valve_control_driver — ESP-specific half of valve_control: the TCA9554
 * I2C relay expander, DI position-sense GPIOs, and the independent
 * hardware-timer watchdog from DRAINMASTER-VALVES.md §7 Rule 3.
 *
 * Board access, per the doc: relays are behind a TCA9554PWR at I2C 0x20
 * (EXIO1-8, not direct GPIO), on GPIO41 (SCL) / GPIO42 (SDA), shared with
 * the RTC. DI1 = GPIO4 (grey), DI2 = GPIO5 (black).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#include "valve_control.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Probes the TCA9554 at 0x20 and configures it all-output, all-off, and
 * configures the DI1/DI2 GPIOs as inputs. Returns an error if the expander
 * doesn't answer — that alone answers bring-up step 1's "does it respond
 * at 0x20". */
esp_err_t valve_control_driver_init(i2c_master_bus_handle_t bus);

/*
 * The one choke point (Rule 1): refuses an unsafe mask via
 * valve_mask_is_safe(), logs it, and drops to all-off instead of writing
 * it. Nothing else may write the expander directly.
 *
 * Arms (or re-arms) the independent 2 s watchdog whenever mask != 0;
 * disarms it when mask == 0. The watchdog runs on esp_timer, not a
 * vTaskDelay in the caller's task, so it still fires even if the calling
 * task is blocked (Rule 3) — see valve_control_release_all().
 */
esp_err_t valve_control_apply(uint8_t mask);

/* Equivalent to valve_control_apply(VALVE_MASK_ALL_OFF). Safe to call from
 * the watchdog callback or anywhere else — it does not touch the caller's
 * task. */
esp_err_t valve_control_release_all(void);

/* Reads the TCA9554's own input register — the actual driven electrical
 * state of the eight relay-driver pins, not our last-commanded value — so
 * a caller can verify a write (or a watchdog release) actually took
 * effect on the wire. */
esp_err_t valve_control_read_relays(uint8_t *mask_out);

/* Position sense: true = the valve is fully closed. The board's DI is
 * ACTIVE-LOW (measured on the real valves 2026-10-16: GPIO 0 when closed), so
 * this inverts the pin when CONFIG_FIREFLY_VALVE_DI_ACTIVE_LOW is set, which
 * is the default. The internal pull-ups are enabled so an unconnected input
 * reads "not closed" rather than floating. */
bool valve_control_read_di(valve_id_t valve);

/* The unmodified GPIO level, before CONFIG_FIREFLY_VALVE_DI_ACTIVE_LOW is
 * applied. The sensor test logs this so polarity can be measured rather
 * than assumed. */
bool valve_control_read_di_raw(valve_id_t valve);

#ifdef __cplusplus
}
#endif
