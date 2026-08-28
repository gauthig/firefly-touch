/*
 * firefly-ble-proxy — "Bluetooth proxy basement".
 *
 * A headless classic-ESP32 node that lives in the coach's basement bay,
 * holds the BLE connections to the Hughes Power Watchdog, the three
 * JBD/Xiaoxiang battery packs and the Renogy MPPT solar charge controller,
 * and re-broadcasts their readings over ESP-NOW so ANY panel can display
 * them.
 *
 * Why a separate node rather than doing this from a panel:
 *   - The Watchdog accepts exactly ONE BLE connection at a time, so
 *     whichever device holds it owns it. Better that be a dedicated node
 *     than a panel that also has a job to do.
 *   - BLE range is the real constraint. The Watchdog is at the shore-power
 *     inlet; the panels are on interior walls, potentially through a steel
 *     bay door. A node sitting next to the device removes range as a
 *     variable entirely.
 *   - Broadcasting means read-only power data costs nothing to add to any
 *     future panel — no extra BLE link, no pairing, no config.
 *   - The battery packs sit in this same bay. Running their links from a
 *     bedroom wall panel (where they started) put a steel bay door in the
 *     RF path and made one panel's radio carry BLE and ESP-NOW at once,
 *     for data every panel would eventually want anyway.
 *
 * This node never receives commands and never actuates anything. It is a
 * pure producer: BLE in, ESP-NOW broadcast out.
 */
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "espnow_link.h"
#include "hughes_wd_client.h"
#include "jbd_bms_client.h"
#include "renogy_solar_client.h"

static const char *TAG = "proxy";

/* Clamps a float to a scaled uint16 for the wire format. Readings are
 * always non-negative here; a negative or absurd value means something is
 * wrong upstream and is better sent as 0 than wrapped into a huge number. */
static uint16_t scale_u16(float value, float scale)
{
    const float v = value * scale;
    if (v <= 0.0f) {
        return 0;
    }
    if (v >= 65535.0f) {
        return 65535;
    }
    return (uint16_t)(v + 0.5f);
}

#if CONFIG_FIREFLY_BATTERY_ENABLED || CONFIG_FIREFLY_SOLAR_ENABLED
/* Signed twin of scale_u16(), for the readings that genuinely go negative:
 * battery current is bidirectional (positive = charging) and a temperature
 * below freezing is a real value, so neither can use the
 * clamp-negatives-to-zero version. */
static int16_t scale_i16(float value, float scale)
{
    const float v = value * scale;
    if (v <= -32768.0f) {
        return -32768;
    }
    if (v >= 32767.0f) {
        return 32767;
    }
    return (int16_t)(v < 0.0f ? v - 0.5f : v + 0.5f);
}
#endif

#if CONFIG_FIREFLY_WD_ENABLED
static void broadcast_shore_power(void)
{
    espnow_telem_msg_t msg = { .kind = ESPNOW_TELEM_SHORE_POWER };
    espnow_shore_power_t *sp = &msg.shore;

    uint8_t lines = 0;
    for (uint8_t line = 1; line <= HUGHES_WD_MAX_LINES; line++) {
        hughes_wd_reading_t r;
        if (!hughes_wd_get_reading(line, &r)) {
            continue;
        }
        const uint8_t idx = line - 1;
        sp->volts_dv[idx] = scale_u16(r.voltage_v, 10.0f);
        sp->amps_da[idx]  = scale_u16(r.current_a, 10.0f);
        sp->watts[idx]    = scale_u16(r.power_w, 1.0f);
        /* Frequency and the error code are device-wide rather than
         * per-line; take them from whichever line reported last. */
        sp->frequency_chz = scale_u16(r.frequency_hz, 100.0f);
        if (r.error_code != 0) {
            sp->error_code = r.error_code;
        }
        lines++;
    }

    /* Nothing valid: stay quiet rather than broadcasting a frame full of
     * zeroes, which a panel would render as a real 0 V reading. Panels
     * time out on silence and show "--" instead. */
    if (lines == 0) {
        return;
    }
    sp->line_count = lines;

    espnow_link_send_telemetry(&msg);
}
#endif

#if CONFIG_FIREFLY_BATTERY_ENABLED
/*
 * One frame per pack, not a pre-combined bank.
 *
 * The combining rules (capacity-weighted SOC, summed current, mean voltage)
 * already live in jbd_bms_combine() as pure host-tested C, and the panel
 * links it anyway — summing here as well would fork that logic across two
 * chips. Sending packs separately also keeps the panel's per-pack detail
 * popup fed, which is what identifies a misbehaving physical battery.
 *
 * A configured-but-unreachable pack is broadcast too, flagged offline: that
 * is how a panel tells "this pack dropped" apart from "the whole proxy is
 * gone", which plain silence cannot express.
 */
static void broadcast_batteries(void)
{
    for (uint8_t slot = 0; slot < JBD_BMS_MAX_BATTERIES; slot++) {
        if (!jbd_bms_slot_configured(slot)) {
            /* Never-configured slots stay silent entirely — broadcasting
             * them would make a panel count phantom packs toward its
             * "N of M" indicator. */
            continue;
        }

        espnow_telem_msg_t msg = { .kind = ESPNOW_TELEM_BATTERY };
        espnow_battery_msg_t *b = &msg.battery;
        b->slot = slot;

        jbd_bms_status_t st;
        if (!jbd_bms_get_status(slot, &st) || !jbd_bms_healthy(slot)) {
            espnow_link_send_telemetry(&msg);   /* flags == 0: offline */
            continue;
        }

        b->flags = ESPNOW_BATTERY_FLAG_ONLINE;
        b->soc_percent = st.soc_percent;
        b->volts_cv = scale_u16(st.voltage_v, 100.0f);
        b->current_da = scale_i16(st.current_a, 10.0f);
        b->residual_dah = scale_u16(st.residual_ah, 10.0f);
        b->full_dah = scale_u16(st.full_capacity_ah, 10.0f);

        if (st.temp_count > 0) {
            float lo = st.temp_c[0];
            float hi = st.temp_c[0];
            for (uint8_t i = 1; i < st.temp_count; i++) {
                if (st.temp_c[i] < lo) {
                    lo = st.temp_c[i];
                }
                if (st.temp_c[i] > hi) {
                    hi = st.temp_c[i];
                }
            }
            b->flags |= ESPNOW_BATTERY_FLAG_TEMP_VALID;
            b->temp_min_dc = scale_i16(lo, 10.0f);
            b->temp_max_dc = scale_i16(hi, 10.0f);
        }

        espnow_link_send_telemetry(&msg);
    }
}

static void log_batteries(void)
{
    for (uint8_t slot = 0; slot < JBD_BMS_MAX_BATTERIES; slot++) {
        if (!jbd_bms_slot_configured(slot)) {
            continue;
        }
        jbd_bms_status_t st;
        if (jbd_bms_get_status(slot, &st) && jbd_bms_healthy(slot)) {
            ESP_LOGI(TAG, "battery %u: %u%% %.2fV %.2fA %.1f/%.1fAh",
                     (unsigned)(slot + 1), (unsigned)st.soc_percent,
                     (double)st.voltage_v, (double)st.current_a,
                     (double)st.residual_ah, (double)st.full_capacity_ah);
        } else {
            ESP_LOGW(TAG, "battery %u: offline", (unsigned)(slot + 1));
        }
    }
}
#endif

#if CONFIG_FIREFLY_SOLAR_ENABLED
/*
 * The solar controller, broadcast whether or not it is reachable.
 *
 * Unlike shore power -- which stays quiet when it has nothing, because a
 * frame of zeroes would render as a real 0 V reading -- this follows the
 * BATTERY contract and sends an offline-flagged frame: a panel showing solar
 * needs to tell "the controller dropped off" from "the whole proxy is gone",
 * and silence cannot express the difference.
 *
 * Temperatures are converted to degrees F HERE rather than on the panel.
 * There is one producer and potentially several consumers, so converting
 * once at the source is the only arrangement in which they cannot disagree
 * -- and it keeps the unit out of the wire format as one more thing to get
 * wrong.
 */
static void broadcast_solar(void)
{
    espnow_telem_msg_t msg = { .kind = ESPNOW_TELEM_SOLAR };
    espnow_solar_msg_t *sol = &msg.solar;

    renogy_solar_status_t st;
    if (!renogy_solar_get_status(&st) || !renogy_solar_healthy()) {
        espnow_link_send_telemetry(&msg);   /* flags == 0: offline */
        return;
    }

    sol->flags = ESPNOW_SOLAR_FLAG_ONLINE;
    sol->charge_state = st.charge_state;
    sol->battery_soc = st.battery_soc;

    sol->battery_volts_cv = scale_u16(st.battery_voltage_v, 100.0f);
    sol->pv_volts_cv      = scale_u16(st.pv_voltage_v, 100.0f);
    sol->pv_current_ca    = scale_u16(st.pv_current_a, 100.0f);
    sol->pv_watts         = scale_u16((float)st.pv_power_w, 1.0f);

    sol->controller_temp_df = scale_i16(renogy_c_to_f(st.controller_temp_c), 10.0f);
    sol->battery_temp_df    = scale_i16(renogy_c_to_f(st.battery_temp_c), 10.0f);

    espnow_link_send_telemetry(&msg);
}

static void log_solar(void)
{
    renogy_solar_status_t st;
    if (renogy_solar_get_status(&st) && renogy_solar_healthy()) {
        ESP_LOGI(TAG, "solar %s (id %u): batt %.1fV %.2fA %u%% | PV %.1fV %.2fA %uW | "
                      "%.0f/%.0f degF | %s",
                 renogy_solar_peer_str(), (unsigned)renogy_solar_device_id(),
                 (double)st.battery_voltage_v, (double)st.battery_current_a,
                 (unsigned)st.battery_soc,
                 (double)st.pv_voltage_v, (double)st.pv_current_a,
                 (unsigned)st.pv_power_w,
                 (double)renogy_c_to_f(st.controller_temp_c),
                 (double)renogy_c_to_f(st.battery_temp_c),
                 renogy_charge_state_str(st.charge_state));
    } else {
        ESP_LOGW(TAG, "solar: offline (peer %s)", renogy_solar_peer_str());
    }
}
#endif

/* Periodic one-line health summary, so a serial capture during bench or
 * bay testing shows at a glance whether the BLE side is alive. */
static void log_status(void)
{
#if CONFIG_FIREFLY_WD_ENABLED
    hughes_wd_reading_t l1, l2;
    const bool have1 = hughes_wd_get_reading(1, &l1);
    const bool have2 = hughes_wd_get_reading(2, &l2);

    if (!have1 && !have2) {
        ESP_LOGW(TAG, "watchdog: no data (peer %s)", hughes_wd_peer_str());
    } else
    if (have1 && have2) {
        ESP_LOGI(TAG, "watchdog %s: L1 %.1fV %.1fA %.0fW | L2 %.1fV %.1fA %.0fW | %.2fHz %s",
                 hughes_wd_peer_str(),
                 (double)l1.voltage_v, (double)l1.current_a, (double)l1.power_w,
                 (double)l2.voltage_v, (double)l2.current_a, (double)l2.power_w,
                 (double)l1.frequency_hz, hughes_wd_error_str(l1.error_code));
    } else {
        const hughes_wd_reading_t *r = have1 ? &l1 : &l2;
        ESP_LOGI(TAG, "watchdog %s: L%u %.1fV %.1fA %.0fW | %.2fHz %s (30A service)",
                 hughes_wd_peer_str(), (unsigned)r->line,
                 (double)r->voltage_v, (double)r->current_a, (double)r->power_w,
                 (double)r->frequency_hz, hughes_wd_error_str(r->error_code));
    }
#endif
#if CONFIG_FIREFLY_BATTERY_ENABLED
    log_batteries();
#endif
#if CONFIG_FIREFLY_SOLAR_ENABLED
    log_solar();
#endif
}

/*
 * Own task rather than a FreeRTOS timer callback, deliberately: the status
 * line formats several doubles, and vararg float formatting in newlib
 * overflows the shared "Tmr Svc" task's 2048-byte stack outright (confirmed
 * on hardware -- it reboot-looped with a stack-overflow panic). Periodic
 * work with any real stack appetite belongs in a task sized for it, not in
 * the timer service, which every other timer in the system shares.
 */
#define PROXY_TASK_STACK 4096
#define PROXY_TASK_PRIO  5

static void proxy_task(void *arg)
{
    (void)arg;
    /* Log roughly every 30 s regardless of the broadcast cadence, so the
     * serial output stays readable when the interval is short. */
    const uint32_t log_every = (30000u / CONFIG_FIREFLY_TELEMETRY_INTERVAL_MS) + 1u;
#if CONFIG_FIREFLY_BATTERY_ENABLED
    /* Batteries go out on their own, much slower cadence: the packs are only
     * polled every FIREFLY_BATTERY_POLL_INTERVAL_MS, so re-broadcasting them
     * at the shore-power rate would just resend identical numbers six times
     * over. Shore power really does change second to second; a 300 Ah bank's
     * state of charge does not. */
    const uint32_t battery_every =
        (CONFIG_FIREFLY_BATTERY_POLL_INTERVAL_MS / CONFIG_FIREFLY_TELEMETRY_INTERVAL_MS) + 1u;
    uint32_t battery_tick = 0;
#endif
#if CONFIG_FIREFLY_SOLAR_ENABLED
    /* Same reasoning as the batteries: the controller is only polled every
     * FIREFLY_SOLAR_POLL_INTERVAL_MS, so re-broadcasting at the shore-power
     * rate would just resend identical numbers several times over. */
    const uint32_t solar_every =
        (CONFIG_FIREFLY_SOLAR_POLL_INTERVAL_MS / CONFIG_FIREFLY_TELEMETRY_INTERVAL_MS) + 1u;
    uint32_t solar_tick = 0;
#endif
    uint32_t tick = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_FIREFLY_TELEMETRY_INTERVAL_MS));
#if CONFIG_FIREFLY_WD_ENABLED
        broadcast_shore_power();
#endif
#if CONFIG_FIREFLY_BATTERY_ENABLED
        if (++battery_tick >= battery_every) {
            battery_tick = 0;
            broadcast_batteries();
        }
#endif
#if CONFIG_FIREFLY_SOLAR_ENABLED
        if (++solar_tick >= solar_every) {
            solar_tick = 0;
            broadcast_solar();
        }
#endif
        if (++tick >= log_every) {
            tick = 0;
            log_status();
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Bluetooth proxy basement starting");

    /* ESP-NOW first: it owns nvs_flash_init() and brings up WiFi, and the
     * coexistence manager prefers WiFi to be up before BLE starts. */
    ESP_ERROR_CHECK(espnow_link_init(ESPNOW_ROLE_TELEMETRY));

#if CONFIG_FIREFLY_WD_ENABLED
    ESP_ERROR_CHECK(hughes_wd_client_start());
#else
    ESP_LOGW(TAG, "Power Watchdog support disabled in Kconfig");
#endif

#if CONFIG_FIREFLY_BATTERY_ENABLED
    /* Shares the Bluedroid stack whichever client starts first brought up.
     * Each battery is its own GATTC app with its own app_id, and every
     * handler filters on the event's remote_bda, so the two clients' events
     * never cross. */
    ESP_ERROR_CHECK(jbd_bms_client_start());
#else
    ESP_LOGW(TAG, "battery monitoring disabled in Kconfig");
#endif

#if CONFIG_FIREFLY_SOLAR_ENABLED
    /* Third client family on the shared stack. It and the Watchdog both
     * discover by advertised name, which is why ble_host arbitrates the
     * single GAP scan -- see components/ble_host/include/ble_host.h. */
    ESP_ERROR_CHECK(renogy_solar_client_start());
#else
    ESP_LOGW(TAG, "solar monitoring disabled in Kconfig");
#endif

    if (xTaskCreate(proxy_task, "proxy", PROXY_TASK_STACK, NULL,
                    PROXY_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create proxy task");
        return;
    }

    ESP_LOGI(TAG, "up on channel %d: shore power every %d ms, batteries every %d ms, "
                  "solar every %d ms",
             CONFIG_FIREFLY_ESPNOW_CHANNEL, CONFIG_FIREFLY_TELEMETRY_INTERVAL_MS,
             CONFIG_FIREFLY_BATTERY_POLL_INTERVAL_MS,
             CONFIG_FIREFLY_SOLAR_POLL_INTERVAL_MS);
}
