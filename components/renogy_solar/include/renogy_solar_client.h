/*
 * renogy_solar_client — Bluedroid GATT-client for a Renogy MPPT charge
 * controller reached through a Renogy BT-1/BT-2 BLE module.
 *
 * Third BLE client on the basement proxy, after components/jbd_bms (the
 * three battery packs) and components/hughes_watchdog (shore power). How it
 * differs from those two:
 *
 *  1. It is REQUEST/RESPONSE, not a stream and not a vendor poll command.
 *     The transport is Modbus RTU tunnelled over BLE — see
 *     renogy_solar_protocol.h. Nothing arrives until we ask.
 *  2. It writes and notifies on characteristics in TWO DIFFERENT SERVICES
 *     (write 0xFFD1 in 0xFFD0, notify 0xFFF1 in 0xFFF0), so discovery walks
 *     every service rather than searching for one.
 *  3. The Modbus device id behind the BT-2's RS485 bridge is not knowable in
 *     advance, and a wrong one produces SILENCE rather than an error. The
 *     client therefore probes: it starts from the configured id and, after
 *     a couple of unanswered polls, moves to the next known candidate,
 *     logging the one that finally answers.
 *
 * Discovery uses ble_host's shared scanner (there is one GAP scan per node
 * and the Watchdog client needs it too), unless CONFIG_FIREFLY_SOLAR_MAC is
 * set to a real address, in which case it connects directly and never scans.
 *
 * Like every other BLE client on this node, it is RECEIVE-ONLY: it reads the
 * controller and never writes a setting. Renogy controllers do accept
 * function-6 writes, but nothing in this project has any business changing a
 * charge profile from a wall panel.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "renogy_solar_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Brings up the shared BLE host, registers the GATTC app, and starts looking
 * for the controller. Safe to call once per boot, after nvs_flash_init().
 * Returns as soon as discovery is under way — connecting, subscribing and
 * the first poll all happen asynchronously.
 */
esp_err_t renogy_solar_client_start(void);

/*
 * Latest reading. Returns false if nothing has been received yet or the last
 * one is stale — same valid/invalid contract as jbd_bms_get_status(),
 * hughes_wd_get_reading() and state_manager_get_tank().
 *
 * The staleness window is DERIVED from the poll interval (3x), not
 * hardcoded: hardcoding it is what made every battery pack read as
 * permanently offline when the JBD poll interval moved to 30 s. Don't
 * reintroduce a constant here.
 */
bool renogy_solar_get_status(renogy_solar_status_t *out);

/* True while connected AND holding a fresh reading. */
bool renogy_solar_healthy(void);

/* True if a controller is configured at all — i.e. whether "no data" means
 * "not fitted" or "the link is down". Mirrors jbd_bms_slot_configured(). */
bool renogy_solar_configured(void);

/* Peer address as a printable string ("--" until connected). Logged at
 * connect time so the MAC can be pinned in Kconfig later if wanted. */
const char *renogy_solar_peer_str(void);

/*
 * The Modbus device id the controller actually answered on, or 0 if it has
 * never answered. Worth surfacing because it is the one piece of
 * configuration that cannot be known before the first successful read.
 */
uint8_t renogy_solar_device_id(void);

#ifdef __cplusplus
}
#endif
