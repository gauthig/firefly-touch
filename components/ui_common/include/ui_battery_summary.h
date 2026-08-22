/*
 * ui_battery_summary — one combined readout for a bank of parallel battery
 * packs, modeled on the Vatrer BMS display's layout and adapted to the
 * panel's 480x800 portrait screen.
 *
 * Replaces the earlier per-pack ui_battery_gauge: the three Vatrer 300 Ah
 * packs are wired in parallel, so the coach has one battery bank, not three
 * batteries, and the screen now reads that way. Aggregation itself lives in
 * jbd_bms_combine() (pure C, host-tested) — this widget only renders the
 * jbd_bms_bank_t it is handed.
 *
 * Layout, top to bottom:
 *   - SOC arc with the percent at its center, indicator colored by band
 *   - 2x2 grid: total voltage / total power / total current / time remaining
 *   - bottom strip: bank high/low temperature in F, and a pack-count
 *     indicator ("3 of 3", amber when a pack is missing)
 *
 * Tapping the widget toggles a per-pack detail popup (MAC, SOC, volts, amps,
 * temperature, online/offline) — the troubleshooting affordance that the old
 * per-pack MAC popup provided, kept alive now that the normal view is
 * combined. Tap the popup to dismiss it.
 */
#pragma once

#include "jbd_bms_protocol.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Per-pack detail as shown in the tap-to-reveal popup. The widget keeps its
 * own copy, so callers may reuse their buffer after the call. */
typedef struct {
    bool             configured;   /* false = Kconfig slot left unset; row hidden */
    bool             online;       /* jbd_bms_healthy() for this slot */
    char             mac[18];
    jbd_bms_status_t status;       /* only meaningful when online */
} ui_battery_pack_info_t;

lv_obj_t *ui_battery_summary_create(lv_obj_t *parent);

/*
 * Push a fresh bank reading. `bank` may be NULL (or carry pack_count == 0)
 * to show "--" everywhere rather than a stale or fabricated zero.
 * `configured_packs` is how many Kconfig slots hold a real MAC, used for the
 * "N of M" indicator. Caller must hold the LVGL lock.
 */
void ui_battery_summary_set_bank(lv_obj_t *summary, const jbd_bms_bank_t *bank,
                                 uint8_t configured_packs);

/*
 * Push per-pack detail for the popup. `packs` is indexed by battery slot;
 * `count` is how many entries it holds. Caller must hold the LVGL lock.
 */
void ui_battery_summary_set_packs(lv_obj_t *summary,
                                  const ui_battery_pack_info_t *packs,
                                  uint8_t count);

/* Show/hide the per-pack detail popup. Caller must hold the LVGL lock. */
void ui_battery_summary_toggle_detail(lv_obj_t *summary);

#ifdef __cplusplus
}
#endif
