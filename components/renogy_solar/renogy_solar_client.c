/*
 * renogy_solar_client — see the header for how this differs from the other
 * two BLE clients on this node.
 *
 * Structure deliberately mirrors components/hughes_watchdog/hughes_wd_client.c
 * and components/jbd_bms/jbd_bms_client.c so all three read the same way:
 * one GATTC app, a small explicit state machine, and a notify handler that
 * reassembles chunks and hands complete frames to the pure-C codec. The poll
 * loop and the device-id probe are the genuinely new pieces.
 */
#include "renogy_solar_client.h"

#include "sdkconfig.h"

/*
 * ⚠️ Compiled away entirely unless the project defines FIREFLY_SOLAR_*.
 *
 * The panel firmware scans components/ as a whole, so this file is built for
 * every panel as well as for the proxy -- but only the proxy's
 * Kconfig.projbuild declares the solar options, and a panel would fail to
 * compile on the missing CONFIG_ symbols.
 *
 * The alternative was to copy the whole solar menu into the panels'
 * Kconfig.projbuild, the way the battery options were copied there when the
 * packs still hung off bedroom_remote. That is dead configuration now: no
 * panel holds this BLE link, and none ever should -- a panel displays solar
 * from the proxy's ESP-NOW broadcast, which needs nothing from this file.
 *
 * Note the split this preserves: renogy_solar_protocol.c stays unconditional
 * and available to panels (types, °C->°F, renogy_charge_state_str() for a
 * future readout), exactly like jbd_bms_protocol.c. Only the ESP-specific
 * CLIENT disappears.
 */
#if CONFIG_FIREFLY_SOLAR_ENABLED

#include <stdio.h>
#include <string.h>

#include "ble_host.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_defs.h"
#include "esp_gattc_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "renogy";

/* Unique across every BLE client on this node -- batteries take 0..2, the
 * Watchdog 10. See BLE_HOST_APP_ID_* in ble_host.h, which is where the
 * allocation lives. */
#define SOLAR_APP_ID BLE_HOST_APP_ID_SOLAR

#define SOLAR_ATTR_RESULT_MAX 8
#define SOLAR_TASK_STACK      4096
#define SOLAR_TASK_PRIO       5

/*
 * A larger MTU turns the 73-byte response from four notifications into one.
 * Not required -- the reassembler handles either -- but fewer chunks means
 * fewer chances for one to go missing and cost a whole poll interval.
 */
#define SOLAR_MTU_REQUEST 247

/*
 * Polls with no answer before moving to the next Modbus device-id candidate.
 * Two rather than one: a single miss is far more likely to be a dropped BLE
 * notification than a wrong address, and rotating the id on every miss would
 * mean never settling on the right one.
 */
#define SOLAR_MISSES_BEFORE_ID_ROTATE 2

/*
 * Poll intervals to sit in CONNECTING or DISCOVERING before giving up and
 * starting over. Generous -- a real connect takes seconds, not a minute --
 * but the cost of being wrong is a needless reconnect, so it errs long.
 */
#define SOLAR_STALL_TICKS 3

/* Log this many complete raw responses at boot so a bench run can check the
 * documented register offsets against the Renogy app's own display. See the
 * TODO(bench) in renogy_solar_protocol.h. */
#define SOLAR_RAW_LOG_FRAMES 3

/* Derived, never hardcoded -- see the header. */
#define SOLAR_STALE_WINDOW_MS (CONFIG_FIREFLY_SOLAR_POLL_INTERVAL_MS * 3)

typedef enum {
    SOLAR_IDLE = 0,
    SOLAR_SCANNING,
    SOLAR_CONNECTING,
    SOLAR_DISCOVERING,
    SOLAR_READY,
} solar_state_t;

static esp_gatt_if_t s_gattc_if = ESP_GATT_IF_NONE;
static solar_state_t s_state;
static esp_bd_addr_t s_bda;
static esp_ble_addr_type_t s_bda_type;
static char     s_peer_str[18] = "--";
static uint16_t s_conn_id;

static uint16_t s_write_svc_start, s_write_svc_end;
static uint16_t s_notify_svc_start, s_notify_svc_end;
static uint16_t s_write_handle, s_notify_handle;

static uint8_t s_buf[RENOGY_CHARGING_RESP_LEN];
static size_t  s_buf_len;
static uint8_t s_raw_logged;

static SemaphoreHandle_t s_lock;
static renogy_solar_status_t s_status;
static bool       s_status_valid;
static TickType_t s_status_tick;

/* Handle into ble_host's shared scanner; -1 when connecting by fixed MAC. */
static int s_scan_handle = -1;

/* Device-id probe state. s_device_id is what we ask with; s_confirmed_id is
 * what the controller has actually answered on (0 = never). */
static uint8_t s_device_id;
static uint8_t s_confirmed_id;
static uint8_t s_misses;

static const esp_bt_uuid_t k_cccd_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid = { .uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG },
};

/* ------------------------------------------------------------ helpers --- */

static bool parse_mac(const char *str, esp_bd_addr_t out)
{
    unsigned b[6];
    if (sscanf(str, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }
    bool all_zero = true;
    for (int i = 0; i < 6; i++) {
        out[i] = (uint8_t)b[i];
        if (out[i] != 0) {
            all_zero = false;
        }
    }
    /* The all-zero placeholder means "not configured", exactly like the
     * battery MACs -- it is not a real address to try connecting to. */
    return !all_zero;
}

/* True if a fixed MAC was configured, in which case discovery is skipped
 * entirely and the shared scanner is never involved. */
static bool have_fixed_mac(void)
{
    esp_bd_addr_t tmp;
    return parse_mac(CONFIG_FIREFLY_SOLAR_MAC, tmp);
}

static void try_connect(void)
{
    ESP_LOGI(TAG, "connecting to %s", s_peer_str);
    s_state = SOLAR_CONNECTING;

    /* Chip portability, learned the hard way on this project: the S3
     * defaults to BLE 5.0 features and does NOT export esp_ble_gattc_open()
     * (link fails with an undefined reference), while the classic ESP32 is
     * BLE 4.2 and has no "enh" variant. Pick by what the build supports
     * rather than by chip -- this component is built for the classic part
     * today, but the panels compile the same tree. */
#if defined(CONFIG_BT_BLE_50_FEATURES_SUPPORTED)
    esp_ble_gatt_creat_conn_params_t params = { 0 };
    memcpy(params.remote_bda, s_bda, sizeof(esp_bd_addr_t));
    params.remote_addr_type = s_bda_type;
    params.own_addr_type    = BLE_ADDR_TYPE_PUBLIC;
    params.is_direct        = true;
    params.is_aux           = false;
    params.phy_mask         = 0x0;
    const esp_err_t err = esp_ble_gattc_enh_open(s_gattc_if, &params);
#else
    const esp_err_t err = esp_ble_gattc_open(s_gattc_if, s_bda, s_bda_type, true);
#endif
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "gattc open failed: %s", esp_err_to_name(err));
        s_state = SOLAR_IDLE;
    }
}

/*
 * Back to looking for the controller. With a fixed MAC there is nothing to
 * look for, so this just retries the connection; otherwise ble_host is asked
 * for another scan. Either way the poll task drives the retry, so a failure
 * here costs one poll interval rather than wedging the client.
 */
static void restart_discovery(void)
{
    s_state = SOLAR_SCANNING;
    s_buf_len = 0;
    s_write_handle = 0;
    s_notify_handle = 0;

    if (s_scan_handle >= 0) {
        ble_host_scan_want(s_scan_handle, true);
    } else {
        try_connect();
    }
}

static void store_status(const renogy_solar_status_t *st)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status = *st;
    s_status_valid = true;
    s_status_tick = xTaskGetTickCount();
    xSemaphoreGive(s_lock);
}

/*
 * Reassembles notification chunks into one complete Modbus response.
 *
 * No header resync is needed here, unlike the Watchdog's stream: exactly one
 * request is outstanding at a time and the buffer is cleared before each one
 * is written, so bytes simply accumulate until the expected length is
 * reached. The CRC then decides whether what arrived is real -- anything
 * that got out of step fails it and is dropped, and the next poll starts
 * clean.
 */
static void on_notify_data(const uint8_t *data, size_t len)
{
    if (len == 0) {
        return;
    }
    if (s_buf_len + len > sizeof(s_buf)) {
        /* More than a whole response: we are out of step with the device.
         * Start over rather than parsing a stitched-together frame. */
        ESP_LOGW(TAG, "response overrun (%u + %u), resetting",
                 (unsigned)s_buf_len, (unsigned)len);
        s_buf_len = 0;
        return;
    }

    memcpy(&s_buf[s_buf_len], data, len);
    s_buf_len += len;
    if (s_buf_len < sizeof(s_buf)) {
        return;
    }

    if (s_raw_logged < SOLAR_RAW_LOG_FRAMES) {
        s_raw_logged++;
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, s_buf, sizeof(s_buf), ESP_LOG_INFO);
    }

    renogy_solar_status_t st;
    if (!renogy_parse_charging_info(s_buf, sizeof(s_buf), &st)) {
        ESP_LOGW(TAG, "bad response (length ok, CRC/format not) -- discarding");
        s_buf_len = 0;
        return;
    }
    s_buf_len = 0;

    if (s_confirmed_id != st.device_id) {
        /* The reply carries the controller's REAL address even when 255 was
         * asked for. Log it once: it is the one setting that cannot be known
         * before the first successful read, and pinning it in Kconfig makes
         * every later boot connect without probing. */
        ESP_LOGI(TAG, "controller answering on Modbus device id %u", (unsigned)st.device_id);
        s_confirmed_id = st.device_id;
    }
    s_misses = 0;

    store_status(&st);

    if (s_raw_logged <= SOLAR_RAW_LOG_FRAMES) {
        ESP_LOGI(TAG, "batt %.1fV %.2fA %u%% | PV %.1fV %.2fA %uW | %.0f/%.0f degF | %s",
                 (double)st.battery_voltage_v, (double)st.battery_current_a,
                 (unsigned)st.battery_soc,
                 (double)st.pv_voltage_v, (double)st.pv_current_a,
                 (unsigned)st.pv_power_w,
                 (double)renogy_c_to_f(st.controller_temp_c),
                 (double)renogy_c_to_f(st.battery_temp_c),
                 renogy_charge_state_str(st.charge_state));
    }
}

/* --------------------------------------------------------- discovery ---- */

/*
 * Two callbacks into ble_host's shared scanner rather than a GAP handler of
 * our own -- there is one GAP scan per node and the Watchdog client needs it
 * too. `found` fires once scanning has actually stopped, which is what makes
 * opening a connection from inside it safe.
 */

static bool solar_scan_match(const char *name, void *ctx)
{
    (void)ctx;
    if (s_state != SOLAR_SCANNING) {
        return false;
    }

    /*
     * An exact name if one is configured, and the generic Renogy prefix test
     * only as a fallback. This coach's module is BT-TH-B00E7B91 and the name
     * is known, so matching it exactly is both cheaper and safer than
     * accepting any BT-TH-* device that happens to be within range -- a
     * neighbouring rig in a campground is a real scenario here.
     */
    const char *want = CONFIG_FIREFLY_SOLAR_NAME;
    const bool matched = (want[0] != '\0') ? renogy_name_equals(name, want)
                                           : renogy_name_matches(name);
    if (!matched) {
        return false;
    }
    ESP_LOGI(TAG, "found Renogy module advertising as '%s'", name);
    return true;
}

static void solar_scan_found(const esp_bd_addr_t bda, esp_ble_addr_type_t addr_type,
                             void *ctx)
{
    (void)ctx;
    if (s_state != SOLAR_SCANNING) {
        return;
    }
    memcpy(s_bda, bda, sizeof(esp_bd_addr_t));
    s_bda_type = addr_type;
    snprintf(s_peer_str, sizeof(s_peer_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             s_bda[0], s_bda[1], s_bda[2], s_bda[3], s_bda[4], s_bda[5]);
    ESP_LOGI(TAG, "peer %s (addr type %d)", s_peer_str, s_bda_type);
    try_connect();
}

/* ------------------------------------------------------------- GATTC ---- */

static void finish_discovery(esp_gatt_if_t gattc_if)
{
    esp_gattc_char_elem_t chars[SOLAR_ATTR_RESULT_MAX];
    uint16_t count;

    if (s_write_svc_start == 0 || s_notify_svc_start == 0) {
        ESP_LOGW(TAG, "service 0x%04X and/or 0x%04X missing -- is this really a "
                      "Renogy BT module? closing",
                 RENOGY_WRITE_SERVICE_UUID, RENOGY_NOTIFY_SERVICE_UUID);
        esp_ble_gattc_close(gattc_if, s_conn_id);
        return;
    }

    const esp_bt_uuid_t write_uuid = {
        .len = ESP_UUID_LEN_16, .uuid = { .uuid16 = RENOGY_WRITE_CHAR_UUID },
    };
    count = SOLAR_ATTR_RESULT_MAX;
    if (esp_ble_gattc_get_char_by_uuid(gattc_if, s_conn_id, s_write_svc_start,
                                       s_write_svc_end, write_uuid, chars,
                                       &count) != ESP_GATT_OK || count == 0) {
        ESP_LOGW(TAG, "write characteristic 0x%04X not found -- closing",
                 RENOGY_WRITE_CHAR_UUID);
        esp_ble_gattc_close(gattc_if, s_conn_id);
        return;
    }
    s_write_handle = chars[0].char_handle;

    const esp_bt_uuid_t notify_uuid = {
        .len = ESP_UUID_LEN_16, .uuid = { .uuid16 = RENOGY_NOTIFY_CHAR_UUID },
    };
    count = SOLAR_ATTR_RESULT_MAX;
    if (esp_ble_gattc_get_char_by_uuid(gattc_if, s_conn_id, s_notify_svc_start,
                                       s_notify_svc_end, notify_uuid, chars,
                                       &count) != ESP_GATT_OK || count == 0) {
        ESP_LOGW(TAG, "notify characteristic 0x%04X not found -- closing",
                 RENOGY_NOTIFY_CHAR_UUID);
        esp_ble_gattc_close(gattc_if, s_conn_id);
        return;
    }
    s_notify_handle = chars[0].char_handle;

    esp_ble_gattc_register_for_notify(gattc_if, s_bda, s_notify_handle);
}

static void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                esp_ble_gattc_cb_param_t *p)
{
    if (event == ESP_GATTC_REG_EVT) {
        /* Registration events fan out to every client on the node, so claim
         * only our own app_id: without this check we would adopt whichever
         * gattc_if another client registered last and quietly stop seeing
         * our own events. */
        if (p->reg.app_id != SOLAR_APP_ID) {
            return;
        }
        s_gattc_if = gattc_if;
        ESP_LOGI(TAG, "app registered, gattc_if %d", gattc_if);
        return;
    }
    if (gattc_if != s_gattc_if) {
        return;
    }

    /*
     * Bluedroid delivers connection events to EVERY registered GATTC app,
     * not just the one that opened the connection ("virtual connection").
     * With four other apps on this node -- three batteries and the Watchdog
     * -- an unfiltered handler would adopt their connections as its own.
     * Every event that carries a peer address is checked against ours.
     */
    switch (event) {
    case ESP_GATTC_OPEN_EVT:
        if (memcmp(p->open.remote_bda, s_bda, sizeof(esp_bd_addr_t)) != 0) {
            return;
        }
        if (p->open.status != ESP_GATT_OK) {
            ESP_LOGW(TAG, "open failed, status %d -- retrying", p->open.status);
            restart_discovery();
        }
        break;

    case ESP_GATTC_CONNECT_EVT:
        if (memcmp(p->connect.remote_bda, s_bda, sizeof(esp_bd_addr_t)) != 0) {
            return;
        }
        s_conn_id = p->connect.conn_id;
        s_state = SOLAR_DISCOVERING;
        s_write_svc_start = s_write_svc_end = 0;
        s_notify_svc_start = s_notify_svc_end = 0;
        s_write_handle = s_notify_handle = 0;
        s_buf_len = 0;
        esp_ble_gattc_send_mtu_req(gattc_if, s_conn_id);
        break;

    case ESP_GATTC_DIS_SRVC_CMPL_EVT:
        if (p->dis_srvc_cmpl.conn_id != s_conn_id) {
            return;
        }
        if (p->dis_srvc_cmpl.status != ESP_GATT_OK) {
            ESP_LOGW(TAG, "service discovery failed, status %d", p->dis_srvc_cmpl.status);
            break;
        }
        /* NULL filter: the write and notify characteristics live in two
         * DIFFERENT services, so both handle ranges are collected in one
         * pass rather than searching twice. */
        esp_ble_gattc_search_service(gattc_if, s_conn_id, NULL);
        break;

    case ESP_GATTC_SEARCH_RES_EVT:
        if (p->search_res.conn_id != s_conn_id ||
            p->search_res.srvc_id.uuid.len != ESP_UUID_LEN_16) {
            return;
        }
        if (p->search_res.srvc_id.uuid.uuid.uuid16 == RENOGY_WRITE_SERVICE_UUID) {
            s_write_svc_start = p->search_res.start_handle;
            s_write_svc_end = p->search_res.end_handle;
        } else if (p->search_res.srvc_id.uuid.uuid.uuid16 == RENOGY_NOTIFY_SERVICE_UUID) {
            s_notify_svc_start = p->search_res.start_handle;
            s_notify_svc_end = p->search_res.end_handle;
        }
        break;

    case ESP_GATTC_SEARCH_CMPL_EVT:
        if (p->search_cmpl.conn_id != s_conn_id) {
            return;
        }
        if (p->search_cmpl.status != ESP_GATT_OK) {
            ESP_LOGW(TAG, "service search failed, status %d", p->search_cmpl.status);
            esp_ble_gattc_close(gattc_if, s_conn_id);
            break;
        }
        finish_discovery(gattc_if);
        break;

    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
        if (p->reg_for_notify.status != ESP_GATT_OK) {
            ESP_LOGW(TAG, "notify registration failed, status %d",
                     p->reg_for_notify.status);
            break;
        }
        esp_gattc_descr_elem_t descrs[SOLAR_ATTR_RESULT_MAX];
        uint16_t count = SOLAR_ATTR_RESULT_MAX;
        if (esp_ble_gattc_get_descr_by_char_handle(gattc_if, s_conn_id,
                                                   p->reg_for_notify.handle, k_cccd_uuid,
                                                   descrs, &count) != ESP_GATT_OK ||
            count == 0) {
            ESP_LOGW(TAG, "no CCCD on notify characteristic -- closing");
            esp_ble_gattc_close(gattc_if, s_conn_id);
            break;
        }
        uint8_t enable[2] = { 0x01, 0x00 };
        esp_ble_gattc_write_char_descr(gattc_if, s_conn_id, descrs[0].handle,
                                       sizeof(enable), enable,
                                       ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
        s_state = SOLAR_READY;
        ESP_LOGI(TAG, "subscribed -- polling %s every %d ms",
                 s_peer_str, CONFIG_FIREFLY_SOLAR_POLL_INTERVAL_MS);
        break;
    }

    case ESP_GATTC_NOTIFY_EVT:
        if (p->notify.conn_id != s_conn_id) {
            return;
        }
        on_notify_data(p->notify.value, p->notify.value_len);
        break;

    case ESP_GATTC_DISCONNECT_EVT:
        if (memcmp(p->disconnect.remote_bda, s_bda, sizeof(esp_bd_addr_t)) != 0) {
            return;
        }
        ESP_LOGW(TAG, "disconnected, reason 0x%02x", p->disconnect.reason);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status_valid = false;
        xSemaphoreGive(s_lock);
        restart_discovery();
        break;

    default:
        break;
    }
}

/* ---------------------------------------------------------- poll loop --- */

/*
 * Advances to the next Modbus device-id candidate. A wrong id is answered
 * with silence rather than an error, so without this the client would sit
 * connected and subscribed and simply never produce a reading -- looking
 * exactly like a dead controller.
 */
static void rotate_device_id(void)
{
    size_t idx = 0;
    for (size_t i = 0; i < k_renogy_device_id_candidate_count; i++) {
        if (k_renogy_device_id_candidates[i] == s_device_id) {
            idx = i + 1;
            break;
        }
    }
    if (idx >= k_renogy_device_id_candidate_count) {
        idx = 0;
    }
    s_device_id = k_renogy_device_id_candidates[idx];
    ESP_LOGW(TAG, "no reply on %u polls -- trying Modbus device id %u",
             (unsigned)SOLAR_MISSES_BEFORE_ID_ROTATE, (unsigned)s_device_id);
}

static void send_poll(void)
{
    uint8_t req[RENOGY_REQUEST_LEN];
    const size_t n = renogy_build_read_request(s_device_id, RENOGY_REG_CHARGING_INFO,
                                               RENOGY_CHARGING_WORDS, req);

    /* Clear the reassembly buffer with the request, not on response: that is
     * what keeps a partial answer from one poll out of the next one's frame. */
    s_buf_len = 0;

    const esp_err_t err = esp_ble_gattc_write_char(s_gattc_if, s_conn_id, s_write_handle,
                                                   (uint16_t)n, req,
                                                   /* The reference implementation writes
                                                    * without a response, and the module
                                                    * expects that -- the real reply comes
                                                    * back as a notification. */
                                                   ESP_GATT_WRITE_TYPE_NO_RSP,
                                                   ESP_GATT_AUTH_REQ_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "poll write failed: %s", esp_err_to_name(err));
    }
}

static void solar_task(void *arg)
{
    (void)arg;

    /* True once a request has actually gone out, so the first tick after
     * subscribing cannot be counted as an unanswered poll -- it would rotate
     * the device id off a perfectly good value before ever trying it. */
    bool poll_sent = false;
    /* Ticks spent mid-connect. SCANNING is excluded: ble_host retries that on
     * its own, so counting it would restart a discovery already in progress. */
    uint8_t stalled = 0;

    /* First discovery attempt from the task rather than from _start(), so a
     * fixed-MAC connect cannot run before the GATTC app has registered. */
    vTaskDelay(pdMS_TO_TICKS(1000));
    restart_discovery();

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_FIREFLY_SOLAR_POLL_INTERVAL_MS));

        if (s_state != SOLAR_READY) {
            poll_sent = false;

            /* Nothing else retries this. A failed open leaves the client
             * IDLE, and CONNECTING/DISCOVERING can stall outright if an
             * expected GATTC event never arrives -- either way the task is
             * the only thing still running, so it owns the retry. */
            if (s_state == SOLAR_IDLE) {
                stalled = 0;
                restart_discovery();
            } else if (s_state != SOLAR_SCANNING && ++stalled >= SOLAR_STALL_TICKS) {
                ESP_LOGW(TAG, "stuck mid-connect -- starting over");
                stalled = 0;
                if (s_conn_id != 0) {
                    esp_ble_gattc_close(s_gattc_if, s_conn_id);
                }
                restart_discovery();
            }
            continue;
        }

        stalled = 0;
        if (s_write_handle == 0) {
            continue;
        }

        /*
         * Count a poll that produced nothing, here rather than in the notify
         * path, so an address answered by SILENCE is caught the same way as
         * one answered by a corrupt frame. Only while the id is unconfirmed:
         * once the controller has answered, its real address is known and a
         * later dropout is a link problem, not an addressing one.
         */
        if (poll_sent && s_confirmed_id == 0 && !renogy_solar_healthy()) {
            if (++s_misses >= SOLAR_MISSES_BEFORE_ID_ROTATE) {
                s_misses = 0;
                rotate_device_id();
            }
        }

        send_poll();
        poll_sent = true;
    }
}

/* ----------------------------------------------------------------- API -- */

esp_err_t renogy_solar_client_start(void)
{
    if (!renogy_solar_configured()) {
        ESP_LOGW(TAG, "no controller configured (empty name and placeholder MAC)");
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_device_id = (uint8_t)CONFIG_FIREFLY_SOLAR_DEVICE_ID;
    s_state = SOLAR_IDLE;

    /* Bluedroid is single-tenant for stack bring-up, for the one GAP and one
     * GATTC callback slot, and for the GAP scan. ble_host owns all of it and
     * fans events out; registering or scanning directly here would silently
     * unhook (or cancel) whichever client got there first. */
    ESP_ERROR_CHECK(ble_host_add_gattc_observer(gattc_event_handler));

    if (have_fixed_mac()) {
        parse_mac(CONFIG_FIREFLY_SOLAR_MAC, s_bda);
        /* Public, like the battery packs -- bench-verified there, and a BT-2
         * uses a fixed factory address for the same reason. */
        s_bda_type = BLE_ADDR_TYPE_PUBLIC;
        snprintf(s_peer_str, sizeof(s_peer_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 s_bda[0], s_bda[1], s_bda[2], s_bda[3], s_bda[4], s_bda[5]);
        ESP_LOGI(TAG, "using configured MAC %s -- no discovery needed", s_peer_str);
    } else {
        s_scan_handle = ble_host_scan_add_matcher(solar_scan_match, solar_scan_found, NULL);
        if (s_scan_handle < 0) {
            ESP_LOGE(TAG, "no free scan matcher slot");
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_ERROR_CHECK(ble_host_start());
    ESP_ERROR_CHECK(esp_ble_gattc_app_register(SOLAR_APP_ID));

    if (xTaskCreate(solar_task, "renogy", SOLAR_TASK_STACK, NULL,
                    SOLAR_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create poll task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "solar client started (looking for '%s', Modbus id %u)",
             CONFIG_FIREFLY_SOLAR_NAME[0] != '\0' ? CONFIG_FIREFLY_SOLAR_NAME : "any Renogy module",
             (unsigned)s_device_id);
    return ESP_OK;
}

bool renogy_solar_get_status(renogy_solar_status_t *out)
{
    if (out == NULL || s_lock == NULL) {
        return false;
    }
    bool ok = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_status_valid &&
        (xTaskGetTickCount() - s_status_tick) < pdMS_TO_TICKS(SOLAR_STALE_WINDOW_MS)) {
        *out = s_status;
        ok = true;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

bool renogy_solar_healthy(void)
{
    renogy_solar_status_t st;
    return s_state == SOLAR_READY && renogy_solar_get_status(&st);
}

bool renogy_solar_configured(void)
{
    return CONFIG_FIREFLY_SOLAR_NAME[0] != '\0' || have_fixed_mac();
}

const char *renogy_solar_peer_str(void)
{
    return s_peer_str;
}

uint8_t renogy_solar_device_id(void)
{
    return s_confirmed_id;
}

#else  /* !CONFIG_FIREFLY_SOLAR_ENABLED */

/*
 * Stubs for builds with no solar options configured (every panel today).
 * They report "not configured" rather than "offline", which is the same
 * distinction jbd_bms_slot_configured() draws -- a caller can tell a missing
 * feature from a broken link.
 */
esp_err_t renogy_solar_client_start(void) { return ESP_ERR_NOT_SUPPORTED; }
bool renogy_solar_get_status(renogy_solar_status_t *out) { (void)out; return false; }
bool renogy_solar_healthy(void) { return false; }
bool renogy_solar_configured(void) { return false; }
const char *renogy_solar_peer_str(void) { return "--"; }
uint8_t renogy_solar_device_id(void) { return 0; }

#endif /* CONFIG_FIREFLY_SOLAR_ENABLED */
