/*
 * firefly-ble-proxy — "Bluetooth proxy basement".
 *
 * A headless classic-ESP32 node that lives in the coach's basement bay,
 * holds the BLE connection to the Hughes Power Watchdog, and re-broadcasts
 * its readings over ESP-NOW so ANY panel can display them.
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
        return;
    }
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
    uint32_t tick = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_FIREFLY_TELEMETRY_INTERVAL_MS));
#if CONFIG_FIREFLY_WD_ENABLED
        broadcast_shore_power();
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

    if (xTaskCreate(proxy_task, "proxy", PROXY_TASK_STACK, NULL,
                    PROXY_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create proxy task");
        return;
    }

    ESP_LOGI(TAG, "up: broadcasting every %d ms on channel %d",
             CONFIG_FIREFLY_TELEMETRY_INTERVAL_MS, CONFIG_FIREFLY_ESPNOW_CHANNEL);
}
