/*
 * firefly-gateway — SIX-LINK test image (#98 step 2b, issue #105).
 *
 *   - Board: chip, flash, PSRAM, MAC, so the hardware is what we think it is.
 *   - BLE: the four real clients, holding all six devices from one board --
 *     3 JBD packs, the Hughes Power Watchdog and the Renogy solar controller
 *     as held links, plus the EasyTouch thermostat poll-and-release. Every
 *     connect goes through ble_host's arbiter (one LE attempt per node).
 *     Each is enabled separately in Kconfig.
 *   - CAN: TWAI in LISTEN-ONLY mode -- never transmits, not even ACK bits --
 *     with the shared bus_survey core counting what it hears.
 *
 * It publishes nothing: no CAN transmit, no ESP-NOW. Its job is a log that
 * answers "does one board hold all six links, for hours?".
 *
 * ⚠️ Run it with the basement proxy and hvac_panel POWERED OFF. The Watchdog
 * and the packs accept one central at a time, and two thermostat pollers
 * collide -- a link the gateway can't get because another node holds it
 * reads exactly like a link it can't hold.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/twai.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "ble_host.h"
#include "bus_survey.h"
#include "easytouch_client.h"
#include "hughes_wd_client.h"
#include "jbd_bms_client.h"
#include "renogy_solar_client.h"

static const char *TAG = "gw";

#define REPORT_MS     ((uint32_t)CONFIG_FIREFLY_GW_REPORT_S * 1000u)
#define SURVEY_CAP    256u

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* -------------------------------------------------------------- board -- */

static void report_board(void)
{
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    ESP_LOGI(TAG, "BOARD ESP32-S3 rev v%d.%d, %d cores, flash %" PRIu32 " MB, PSRAM %u KB, IDF %s",
             ci.revision / 100, ci.revision % 100, ci.cores, flash / (1024u * 1024u),
             (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024u), esp_get_idf_version());
    ESP_LOGI(TAG, "BOARD base MAC %02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);
}

/* ---------------------------------------------------------------- CAN -- */

static survey_t        s_sv;
static survey_entry_t *s_tab;
static bool            s_can_up;

static void can_task(void *arg)
{
    (void)arg;
    twai_message_t m;
    for (;;) {
        if (twai_receive(&m, pdMS_TO_TICKS(1000)) != ESP_OK) {
            continue;
        }
        if (!m.extd || m.rtr) {
            continue;
        }
        survey_note(&s_sv, now_ms(), (m.identifier >> 8) & 0x1FFFFu, (uint8_t)(m.identifier & 0xFFu),
                    m.data_length_code, m.data);
    }
}

static void can_start(void)
{
    s_tab = heap_caps_calloc(SURVEY_CAP, sizeof(survey_entry_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_tab == NULL) {
        ESP_LOGE(TAG, "CAN: no memory for the survey table");
        return;
    }
    survey_init(&s_sv, s_tab, SURVEY_CAP, now_ms());

    /* LISTEN_ONLY: the controller never drives the bus, not even ACK bits. */
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(CONFIG_FIREFLY_GW_CAN_TX_GPIO,
                                                          CONFIG_FIREFLY_GW_CAN_RX_GPIO,
                                                          TWAI_MODE_LISTEN_ONLY);
    g.rx_queue_len = 32;
    const twai_timing_config_t t = TWAI_TIMING_CONFIG_250KBITS();
    const twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    esp_err_t err = twai_driver_install(&g, &t, &f);
    if (err == ESP_OK) {
        err = twai_start();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CAN: start failed (%s)", esp_err_to_name(err));
        return;
    }
    s_can_up = true;
    xTaskCreatePinnedToCore(can_task, "can_rx", 4096, NULL, 10, NULL, 0);
    ESP_LOGI(TAG, "CAN: listen-only at 250 kbps, TX=GPIO%d RX=GPIO%d (never transmits)",
             CONFIG_FIREFLY_GW_CAN_TX_GPIO, CONFIG_FIREFLY_GW_CAN_RX_GPIO);
}

static void report_can(void)
{
    if (!s_can_up) {
        ESP_LOGW(TAG, "CAN: not running");
        return;
    }
    twai_status_info_t st = { 0 };
    twai_get_status_info(&st);
    ESP_LOGI(TAG, "CAN: state=%d frames=%" PRIu32 " keys=%u missed=%" PRIu32 " overrun=%" PRIu32
             " buserr=%" PRIu32 " rxerr=%" PRIu32,
             (int)st.state, s_sv.total_frames, (unsigned)s_sv.used, st.rx_missed_count,
             st.rx_overrun_count, st.bus_error_count, st.rx_error_counter);
    for (size_t i = 0; i < s_sv.used && i < 24u; i++) {
        const survey_entry_t *e = &s_tab[i];
        const uint32_t avg = (e->count > 1) ? (e->last_ms - e->first_ms) / (e->count - 1u) : 0u;
        ESP_LOGI(TAG, "  CAN %05" PRIX32 " sa=%02X b0=%02X n=%" PRIu32 " avg=%" PRIu32 "ms",
                 e->dgn, e->sa, e->b0, e->count, avg);
    }
}

/* ---------------------------------------------------------------- BLE -- *
 *
 * The four real clients do the work; this file only starts them and watches.
 * The watching is the point of this image (#98 step 2b): over hours, does one
 * board hold all six links -- and when a link drops, does it come back?
 */

typedef enum {
    D_PACK1 = 0, D_PACK2, D_PACK3, D_WATCHDOG, D_SOLAR, D_THERMOSTAT, D_COUNT,
} dev_t_;

static const char *const k_dev_name[D_COUNT] = {
    "pack 1", "pack 2", "pack 3", "watchdog", "solar", "thermostat",
};

typedef struct {
    bool          known;        /* bda learned */
    esp_bd_addr_t bda;
    bool          connected;
    uint32_t      connects;
    uint32_t      drops;
    int           last_reason;  /* -1 = none yet */
    int8_t        rssi;
    bool          rssi_valid;
    uint32_t      last_change_ms;
} link_stat_t;

static link_stat_t   s_link[D_COUNT];
static portMUX_TYPE  s_link_mux = portMUX_INITIALIZER_UNLOCKED;

static bool parse_mac(const char *s, uint8_t out[6])
{
    unsigned v[6];
    if (s == NULL || sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        return false;
    }
    bool nonzero = false;
    for (int i = 0; i < 6; i++) {
        out[i] = (uint8_t)v[i];
        nonzero |= (v[i] != 0);
    }
    return nonzero;
}

/* The Watchdog and solar addresses are learned by their clients' scans, so
 * they are picked up from the clients' peer strings as they appear. */
static void learn_peer(dev_t_ d, const char *peer)
{
    esp_bd_addr_t bda;
    if (s_link[d].known || !parse_mac(peer, bda)) {
        return;
    }
    portENTER_CRITICAL(&s_link_mux);
    memcpy(s_link[d].bda, bda, sizeof(bda));
    s_link[d].known = true;
    portEXIT_CRITICAL(&s_link_mux);
}

/* Anything connecting that is not a pack, the Watchdog or solar is taken to
 * be the thermostat: it is the only other client on this node, and its
 * client has no peer accessor. Learned on its first connect -- which is only
 * sound because every other device's address is known before it connects
 * (packs, a pinned Watchdog and a fixed solar MAC are parsed at start-up). */
static int dev_for_bda(const uint8_t *bda, bool learn_unknown)
{
    for (int d = 0; d < D_COUNT; d++) {
        if (s_link[d].known && memcmp(s_link[d].bda, bda, sizeof(esp_bd_addr_t)) == 0) {
            return d;
        }
    }
    if (learn_unknown && !s_link[D_THERMOSTAT].known) {
        memcpy(s_link[D_THERMOSTAT].bda, bda, sizeof(esp_bd_addr_t));
        s_link[D_THERMOSTAT].known = true;
        return D_THERMOSTAT;
    }
    return -1;
}

/*
 * Connection bookkeeping. Bluedroid delivers CONNECT/DISCONNECT once per
 * registered GATTC app ("virtual connection"), i.e. six copies per event on
 * this node -- so counting is on state TRANSITIONS, never per event.
 */
static void gattc_stats_observer(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                 esp_ble_gattc_cb_param_t *p)
{
    (void)gattc_if;
    if (event == ESP_GATTC_CONNECT_EVT) {
        portENTER_CRITICAL(&s_link_mux);
        const int d = dev_for_bda(p->connect.remote_bda, true);
        if (d >= 0 && !s_link[d].connected) {
            s_link[d].connected = true;
            s_link[d].connects++;
            s_link[d].last_change_ms = now_ms();
        }
        portEXIT_CRITICAL(&s_link_mux);
    } else if (event == ESP_GATTC_DISCONNECT_EVT) {
        portENTER_CRITICAL(&s_link_mux);
        const int d = dev_for_bda(p->disconnect.remote_bda, false);
        if (d >= 0 && s_link[d].connected) {
            s_link[d].connected = false;
            s_link[d].drops++;
            s_link[d].last_reason = (int)p->disconnect.reason;
            s_link[d].rssi_valid = false;
            s_link[d].last_change_ms = now_ms();
        }
        portEXIT_CRITICAL(&s_link_mux);
    }
}

static void gap_rssi_observer(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *p)
{
    if (event != ESP_GAP_BLE_READ_RSSI_COMPLETE_EVT ||
        p->read_rssi_cmpl.status != ESP_BT_STATUS_SUCCESS) {
        return;
    }
    portENTER_CRITICAL(&s_link_mux);
    const int d = dev_for_bda(p->read_rssi_cmpl.remote_addr, false);
    if (d >= 0 && s_link[d].connected) {
        s_link[d].rssi = p->read_rssi_cmpl.rssi;
        s_link[d].rssi_valid = true;
    }
    portEXIT_CRITICAL(&s_link_mux);
}

/* Ask for a fresh RSSI on every live link; answers land in the observer and
 * show up in the next report. */
static void request_rssi(void)
{
    for (int d = 0; d < D_COUNT; d++) {
        esp_bd_addr_t bda;
        portENTER_CRITICAL(&s_link_mux);
        const bool live = s_link[d].connected;
        memcpy(bda, s_link[d].bda, sizeof(bda));
        portEXIT_CRITICAL(&s_link_mux);
        if (live) {
            esp_ble_gap_read_rssi(bda);
        }
    }
}

static void ble_start(void)
{
    /* Observers first, so not even the first connection is missed. ble_host
     * accepts them before the stack is up. */
    ESP_ERROR_CHECK(ble_host_add_gattc_observer(gattc_stats_observer));
    ESP_ERROR_CHECK(ble_host_add_gap_observer(gap_rssi_observer));

    for (int d = 0; d < D_COUNT; d++) {
        s_link[d].last_reason = -1;
    }

#if CONFIG_FIREFLY_BATTERY_ENABLED
    const char *macs[3] = {
        CONFIG_FIREFLY_BATTERY_1_MAC, CONFIG_FIREFLY_BATTERY_2_MAC, CONFIG_FIREFLY_BATTERY_3_MAC,
    };
    for (int i = 0; i < 3; i++) {
        s_link[D_PACK1 + i].known = parse_mac(macs[i], s_link[D_PACK1 + i].bda);
    }
    ESP_ERROR_CHECK(jbd_bms_client_start());
#endif
#if CONFIG_FIREFLY_WD_ENABLED
    s_link[D_WATCHDOG].known = parse_mac(CONFIG_FIREFLY_WD_MAC, s_link[D_WATCHDOG].bda);
    ESP_ERROR_CHECK(hughes_wd_client_start());
#endif
#if CONFIG_FIREFLY_SOLAR_ENABLED
    /* Known up front when the MAC is fixed. It must be: solar can connect
     * before the first report learns it from the client, and the observer
     * would otherwise file that connect under the thermostat. */
    s_link[D_SOLAR].known = parse_mac(CONFIG_FIREFLY_SOLAR_MAC, s_link[D_SOLAR].bda);
    ESP_ERROR_CHECK(renogy_solar_client_start());
#endif
#if CONFIG_FIREFLY_EASYTOUCH_ENABLED
    ESP_ERROR_CHECK(easytouch_client_start());
#endif
}

/* One line per device: link state from the observer, plus the client's own
 * view (healthy = a reading fresher than its staleness window). */
static void report_ble(void)
{
#if CONFIG_FIREFLY_WD_ENABLED
    learn_peer(D_WATCHDOG, hughes_wd_peer_str());
#endif
#if CONFIG_FIREFLY_SOLAR_ENABLED
    learn_peer(D_SOLAR, renogy_solar_peer_str());
#endif

    link_stat_t snap[D_COUNT];
    portENTER_CRITICAL(&s_link_mux);
    memcpy(snap, s_link, sizeof(snap));
    portEXIT_CRITICAL(&s_link_mux);

    const uint32_t t = now_ms();
    bool enabled[D_COUNT] = { false };
    bool healthy[D_COUNT] = { false };
    char detail[D_COUNT][96];
    memset(detail, 0, sizeof(detail));

#if CONFIG_FIREFLY_BATTERY_ENABLED
    for (uint8_t i = 0; i < 3; i++) {
        enabled[D_PACK1 + i] = jbd_bms_slot_configured(i);
        jbd_bms_status_t st;
        healthy[D_PACK1 + i] = jbd_bms_get_status(i, &st) && jbd_bms_healthy(i);
        if (healthy[D_PACK1 + i]) {
            snprintf(detail[D_PACK1 + i], sizeof(detail[0]), "%u%% %.2fV %.2fA",
                     (unsigned)st.soc_percent, (double)st.voltage_v, (double)st.current_a);
        }
    }
#endif
#if CONFIG_FIREFLY_WD_ENABLED
    enabled[D_WATCHDOG] = true;
    healthy[D_WATCHDOG] = hughes_wd_healthy();
    if (healthy[D_WATCHDOG]) {
        int n = 0;
        for (uint8_t line = 1; line <= HUGHES_WD_MAX_LINES; line++) {
            hughes_wd_reading_t r;
            if (hughes_wd_get_reading(line, &r) && n < (int)sizeof(detail[0])) {
                n += snprintf(detail[D_WATCHDOG] + n, sizeof(detail[0]) - (size_t)n,
                              "L%u %.1fV %.1fA ", (unsigned)line, (double)r.voltage_v,
                              (double)r.current_a);
            }
        }
    }
#endif
#if CONFIG_FIREFLY_SOLAR_ENABLED
    enabled[D_SOLAR] = renogy_solar_configured();
    renogy_solar_status_t ss;
    healthy[D_SOLAR] = renogy_solar_get_status(&ss) && renogy_solar_healthy();
    if (healthy[D_SOLAR]) {
        snprintf(detail[D_SOLAR], sizeof(detail[0]), "batt %.1fV PV %.1fV %uW %s",
                 (double)ss.battery_voltage_v, (double)ss.pv_voltage_v, (unsigned)ss.pv_power_w,
                 renogy_charge_state_str(ss.charge_state));
    }
#endif
#if CONFIG_FIREFLY_EASYTOUCH_ENABLED
    enabled[D_THERMOSTAT] = true;
    static easytouch_status_t es;   /* large: keep it off the stack */
    healthy[D_THERMOSTAT] = easytouch_client_get_status(&es) && easytouch_client_healthy();
    if (healthy[D_THERMOSTAT]) {
        int n = snprintf(detail[D_THERMOSTAT], sizeof(detail[0]), "%u zone(s):",
                         (unsigned)es.zone_count);
        for (uint8_t z = 0; z < EASYTOUCH_MAX_ZONES && n < (int)sizeof(detail[0]); z++) {
            if (es.zones[z].present) {
                n += snprintf(detail[D_THERMOSTAT] + n, sizeof(detail[0]) - (size_t)n,
                              " %dF/m%u", (int)es.zones[z].inside_f, (unsigned)es.zones[z].mode);
            }
        }
    }
#endif

    int up = 0;
    int total = 0;
    for (int d = 0; d < D_COUNT; d++) {
        if (!enabled[d]) {
            ESP_LOGI(TAG, "  %-10s disabled / not configured", k_dev_name[d]);
            continue;
        }
        total++;
        up += healthy[d] ? 1 : 0;
        const link_stat_t *s = &snap[d];
        char rssi[12] = "--";
        if (s->rssi_valid) {
            snprintf(rssi, sizeof(rssi), "%d", s->rssi);
        }
        char reason[12] = "-";
        if (s->last_reason >= 0) {
            snprintf(reason, sizeof(reason), "0x%02X", (unsigned)s->last_reason);
        }
        const char *link = s->connected ? "LINK" : "----";
        /* The thermostat is poll-and-release: "----" between polls is normal,
         * health is judged by its readings, not by a held link. */
        ESP_LOGI(TAG, "  %-10s %s %s rssi %4s conn %3" PRIu32 " drop %3" PRIu32 " last %s %4" PRIu32
                      "s ago | %s",
                 k_dev_name[d], healthy[d] ? "OK  " : "DOWN", link, rssi, s->connects, s->drops,
                 reason, s->last_change_ms ? (t - s->last_change_ms) / 1000u : 0u, detail[d]);
    }

    ble_host_conn_stats_t cs;
    ble_host_conn_get_stats(&cs);
    ESP_LOGI(TAG, "BLE: %d/%d devices healthy | opens %" PRIu32 " queued %" PRIu32 " failed %" PRIu32
                  " timeouts %" PRIu32 " | now %s, %u waiting",
             up, total, cs.started, cs.queued, cs.failed, cs.timeouts, cs.busy ? "connecting" : "idle",
             (unsigned)cs.queue_depth);

    request_rssi();
}

/* --------------------------------------------------------------- main -- */

void app_main(void)
{
    ESP_LOGW(TAG, "firefly-gateway SIX-LINK test image (#98 step 2b): BLE clients live, "
                  "CAN listen-only, no ESP-NOW, publishes nothing");
    report_board();

    esp_err_t err = nvs_flash_init();   /* Bluedroid needs NVS; ble_host does not init it */
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    can_start();
    ble_start();

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(REPORT_MS));
        ESP_LOGI(TAG, "---- report @ %" PRIu32 " s | heap internal %u B (min %u), PSRAM %u B ----",
                 now_ms() / 1000u, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        report_ble();
        report_can();
    }
}
