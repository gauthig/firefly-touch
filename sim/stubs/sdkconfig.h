/*
 * Simulator stub for sdkconfig.h.
 *
 * ESP-IDF force-includes the generated sdkconfig.h into every translation
 * unit; the plain-CMake sim build has no such file, so this stands in with
 * the handful of CONFIG_ symbols the shared UI sources actually read.
 *
 * Values are plausible fakes, not the real coach's — the point is that the
 * layouts and staleness logic they feed are exercisable in the sim, not
 * that they match hardware.
 */
#pragma once

/* Display labels for the battery bank's per-pack detail popup. The real
 * BLE links live on the basement proxy; a panel only ever labels rows with
 * these. */
#define CONFIG_FIREFLY_BATTERY_1_MAC "AA:BB:CC:DD:EE:01"
#define CONFIG_FIREFLY_BATTERY_2_MAC "AA:BB:CC:DD:EE:02"
#define CONFIG_FIREFLY_BATTERY_3_MAC "AA:BB:CC:DD:EE:03"

/* The panel derives its "pack has gone quiet" window from this (3x). Kept
 * at the firmware default so the sim ages packs out on the same schedule
 * the hardware does. */
#define CONFIG_FIREFLY_BATTERY_POLL_INTERVAL_MS 30000
/* ui.c derives its solar staleness window (3x) from this, the same way
 * it does for the battery packs. */
#define CONFIG_FIREFLY_SOLAR_BROADCAST_INTERVAL_MS 30000
