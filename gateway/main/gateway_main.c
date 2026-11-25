/*
 * firefly-gateway — BRING-UP image (#98 step 2a). Makes no connections:
 *
 *   - Board: chip, flash, PSRAM, MAC, so the hardware is what we think it is.
 *   - CAN: TWAI in LISTEN-ONLY mode -- never transmits, not even ACK bits --
 *     with the shared bus_survey core counting what it hears.
 *   - BLE: a scan with no connections, reporting signal strength for the six
 *     devices the gateway will eventually hold. ble_host scans ACTIVELY, so the
 *     radio does send short scan requests (as a phone listing devices does);
 *     it never connects, writes or subscribes. Holding the devices for real
 *     (thermostat auth/status/commands, pack and solar polls) does transmit,
 *     and comes in the next image.
 *
 * Because it connects to nothing it can run beside the proxy and hvac_panel.
 * Note that a device already connected elsewhere usually stops advertising,
 * so the proxy's three packs, Watchdog and solar may only appear while the
 * proxy is powered off.
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
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "ble_host.h"
#include "bus_survey.h"

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

/* ---------------------------------------------------------------- BLE -- */

typedef enum {
    T_NONE = 0, T_PACK1, T_PACK2, T_PACK3, T_WATCHDOG, T_SOLAR, T_THERMOSTAT, T_COUNT,
} target_t;

static const char *const k_target_name[T_COUNT] = {
    "-", "battery pack 1", "battery pack 2", "battery pack 3", "power watchdog", "solar (renogy)",
    "thermostat",
};

typedef struct {
    bool          seen;
    esp_bd_addr_t bda;
    char          name[32];
    int8_t        last, min, max;
    int32_t       sum;
    uint32_t      n;
    uint32_t      last_ms;
} target_stat_t;

static target_stat_t  s_tgt[T_COUNT];
static esp_bd_addr_t  s_pack_mac[3];
static bool           s_pack_cfg[3];
static volatile uint32_t s_adv_total;
static portMUX_TYPE   s_mux = portMUX_INITIALIZER_UNLOCKED;

static bool parse_mac(const char *s, uint8_t out[6])
{
    unsigned v[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        return false;
    }
    bool nonzero = false;
    for (int i = 0; i < 6; i++) {
        out[i] = (uint8_t)v[i];
        nonzero |= (v[i] != 0);
    }
    return nonzero;
}

/* Complete name first, then short name; adv data and scan response together.
 * Non-const: esp_ble_resolve_adv_data_by_type() takes a mutable buffer. */
static void adv_name(esp_ble_gap_cb_param_t *p, char *out, size_t len)
{
    uint8_t n = 0;
    uint8_t *d = esp_ble_resolve_adv_data_by_type(p->scan_rst.ble_adv,
                                                  p->scan_rst.adv_data_len + p->scan_rst.scan_rsp_len,
                                                  ESP_BLE_AD_TYPE_NAME_CMPL, &n);
    if (d == NULL || n == 0) {
        d = esp_ble_resolve_adv_data_by_type(p->scan_rst.ble_adv,
                                             p->scan_rst.adv_data_len + p->scan_rst.scan_rsp_len,
                                             ESP_BLE_AD_TYPE_NAME_SHORT, &n);
    }
    if (d == NULL || n == 0) {
        out[0] = '\0';
        return;
    }
    const size_t c = (n < len - 1u) ? n : len - 1u;
    memcpy(out, d, c);
    out[c] = '\0';
}

static target_t classify(const esp_bd_addr_t bda, const char *name)
{
    for (int i = 0; i < 3; i++) {
        if (s_pack_cfg[i] && memcmp(bda, s_pack_mac[i], 6) == 0) {
            return (target_t)(T_PACK1 + i);
        }
    }
    if (name[0] == '\0') {
        return T_NONE;
    }
    /* Watchdog: SUBSTRING match -- this coach's unit advertises with a leading 'A'
     * ("APMD..."), which a prefix test misses (same rule as hughes_wd_name_matches). */
    if (strstr(name, "PMD") || strstr(name, "PWS") || strstr(name, "PMS")) {
        return T_WATCHDOG;
    }
    if (strncmp(name, CONFIG_FIREFLY_SOLAR_NAME_PREFIX, strlen(CONFIG_FIREFLY_SOLAR_NAME_PREFIX)) == 0) {
        return T_SOLAR;
    }
    if (strncmp(name, CONFIG_FIREFLY_EASYTOUCH_NAME_PREFIX,
                strlen(CONFIG_FIREFLY_EASYTOUCH_NAME_PREFIX)) == 0) {
        return T_THERMOSTAT;
    }
    return T_NONE;
}

static void gap_observer(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *p)
{
    if (event != ESP_GAP_BLE_SCAN_RESULT_EVT || p->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) {
        return;
    }
    s_adv_total++;
    char name[32];
    adv_name(p, name, sizeof(name));
    const target_t t = classify(p->scan_rst.bda, name);
    if (t == T_NONE) {
        return;
    }
    const int8_t rssi = (int8_t)p->scan_rst.rssi;
    portENTER_CRITICAL(&s_mux);
    target_stat_t *s = &s_tgt[t];
    if (!s->seen) {
        s->seen = true;
        s->min = rssi;
        s->max = rssi;
    }
    memcpy(s->bda, p->scan_rst.bda, sizeof(esp_bd_addr_t));
    if (name[0] != '\0') {
        memcpy(s->name, name, sizeof(s->name));
    }
    s->last = rssi;
    s->min = (rssi < s->min) ? rssi : s->min;
    s->max = (rssi > s->max) ? rssi : s->max;
    s->sum += rssi;
    s->n++;
    s->last_ms = now_ms();
    portEXIT_CRITICAL(&s_mux);
}

/* The bring-up never claims a device: returning false keeps ble_host scanning. */
static bool never_match(const char *name, void *ctx)
{
    (void)name;
    (void)ctx;
    return false;
}

static void never_found(const esp_bd_addr_t bda, esp_ble_addr_type_t type, void *ctx)
{
    (void)bda;
    (void)type;
    (void)ctx;
}

static void ble_start(void)
{
    s_pack_cfg[0] = parse_mac(CONFIG_FIREFLY_BATTERY_1_MAC, s_pack_mac[0]);
    s_pack_cfg[1] = parse_mac(CONFIG_FIREFLY_BATTERY_2_MAC, s_pack_mac[1]);
    s_pack_cfg[2] = parse_mac(CONFIG_FIREFLY_BATTERY_3_MAC, s_pack_mac[2]);

    if (ble_host_start() != ESP_OK) {
        ESP_LOGE(TAG, "BLE: host failed to start");
        return;
    }
    ble_host_add_gap_observer(gap_observer);
    const int h = ble_host_scan_add_matcher(never_match, never_found, NULL);
    ble_host_scan_want(h, true);
    ESP_LOGI(TAG, "BLE: scanning (no connections); battery MACs configured: %d of 3",
             (int)s_pack_cfg[0] + (int)s_pack_cfg[1] + (int)s_pack_cfg[2]);
}

static void report_ble(void)
{
    target_stat_t snap[T_COUNT];
    portENTER_CRITICAL(&s_mux);
    memcpy(snap, s_tgt, sizeof(snap));
    portEXIT_CRITICAL(&s_mux);

    const uint32_t t = now_ms();
    ESP_LOGI(TAG, "BLE: %" PRIu32 " advertisements heard so far", s_adv_total);
    for (int i = T_PACK1; i < T_COUNT; i++) {
        const target_stat_t *s = &snap[i];
        if (!s->seen) {
            const bool pack = (i <= T_PACK3);
            ESP_LOGW(TAG, "  %-16s NOT SEEN%s", k_target_name[i],
                     (pack && !s_pack_cfg[i - T_PACK1]) ? " (MAC not configured)" : "");
            continue;
        }
        ESP_LOGI(TAG, "  %-16s rssi last %d avg %d min %d max %d dBm, n=%" PRIu32 ", %" PRIu32
                 " s ago, '%s' %02X:%02X:%02X:%02X:%02X:%02X",
                 k_target_name[i], s->last, (int)(s->sum / (int32_t)s->n), s->min, s->max, s->n,
                 (t - s->last_ms) / 1000u, s->name, s->bda[0], s->bda[1], s->bda[2], s->bda[3],
                 s->bda[4], s->bda[5]);
    }
}

/* --------------------------------------------------------------- main -- */

void app_main(void)
{
    ESP_LOGW(TAG, "firefly-gateway BRING-UP image: no connections (CAN listen-only, BLE scan only)");
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
        ESP_LOGI(TAG, "---- report @ %" PRIu32 " s | heap internal %u B, PSRAM %u B ----",
                 now_ms() / 1000u, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        report_can();
        report_ble();
    }
}
