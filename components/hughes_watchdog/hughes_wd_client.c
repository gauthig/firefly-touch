/*
 * hughes_wd_client — see the header for how this differs from jbd_bms.
 *
 * Structure deliberately mirrors components/jbd_bms/jbd_bms_client.c so the
 * two read the same way: one GATTC app, a small explicit state machine, and
 * a notify handler that reassembles chunks and hands complete frames to the
 * pure-C codec.
 *
 * Discovery is delegated to ble_host's shared scanner. There is exactly one
 * GAP scan per node, so a client that starts and stops it itself works only
 * until a second client does the same -- and then whichever finds its device
 * first cancels the other's scan, silently.
 */
#include "hughes_wd_client.h"

#include <stdio.h>
#include <string.h>

#include "ble_host.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_defs.h"
#include "esp_gattc_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "hughes_wd";

/* Unique across every BLE client on this node -- the batteries take 0..2.
 * See BLE_HOST_APP_ID_* in ble_host.h, which is where the allocation lives. */
#define WD_APP_ID          BLE_HOST_APP_ID_WATCHDOG
/* The device streams ~1 Hz; three missed seconds is already a problem, but
 * allow generous slack for BLE/WiFi coexistence hiccups before declaring a
 * line stale. */
#define WD_STALE_WINDOW_MS 15000
#define WD_ATTR_RESULT_MAX 4

/* Log this many complete raw packets at boot so a bench run can confirm the
 * documented byte layout against the Watchdog's own display. See the
 * TODO(bench) in hughes_wd_protocol.h -- these offsets come from public
 * reverse engineering, not from this unit. */
#define WD_RAW_LOG_PACKETS 5

typedef enum {
    WD_IDLE = 0,
    WD_SCANNING,
    WD_CONNECTING,
    WD_READY,
} wd_state_t;

static esp_gatt_if_t s_gattc_if = ESP_GATT_IF_NONE;
static wd_state_t    s_state;
static esp_bd_addr_t s_bda;
static esp_ble_addr_type_t s_bda_type;
static char          s_peer_str[18] = "--";
static uint16_t      s_conn_id;
static uint16_t      s_svc_start, s_svc_end, s_notify_handle;

static uint8_t  s_buf[HUGHES_WD_PACKET_LEN];
static size_t   s_buf_len;
static uint8_t  s_raw_logged;

static SemaphoreHandle_t s_lock;
static hughes_wd_reading_t s_reading[HUGHES_WD_MAX_LINES];
static bool       s_reading_valid[HUGHES_WD_MAX_LINES];
static TickType_t s_reading_tick[HUGHES_WD_MAX_LINES];

/* Handle into ble_host's shared scanner; -1 until registered. */
static int s_scan_handle = -1;

static const esp_bt_uuid_t k_svc_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid = { .uuid16 = HUGHES_WD_SERVICE_UUID },
};
static const esp_bt_uuid_t k_notify_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid = { .uuid16 = HUGHES_WD_NOTIFY_UUID },
};
static const esp_bt_uuid_t k_cccd_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid = { .uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG },
};

/* ------------------------------------------------------------ helpers --- */

/*
 * "I need an address again." ble_host owns the one GAP scan for the whole
 * node and decides when to actually run it; calling
 * esp_ble_gap_start_scanning() from here would cancel the solar client's
 * scan the moment ours found something.
 */
static void start_scan(void)
{
    s_state = WD_SCANNING;
    s_buf_len = 0;
    ble_host_scan_want(s_scan_handle, true);
}

static void try_connect(void)
{
    ESP_LOGI(TAG, "connecting to %s", s_peer_str);
    s_state = WD_CONNECTING;

    /* Chip portability, learned the hard way on this project: the S3
     * defaults to BLE 5.0 features and does NOT export
     * esp_ble_gattc_open() (link fails with an undefined reference), while
     * the classic ESP32 is BLE 4.2 and does not have the "enh" variant.
     * Pick by what the build actually supports rather than by chip. */
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
        start_scan();
    }
}

static void store_reading(const hughes_wd_reading_t *r)
{
    if (r->line < 1 || r->line > HUGHES_WD_MAX_LINES) {
        return;
    }
    const uint8_t idx = r->line - 1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_reading[idx] = *r;
    s_reading_valid[idx] = true;
    s_reading_tick[idx] = xTaskGetTickCount();
    xSemaphoreGive(s_lock);
}

/* Reassembles the 20-byte notification chunks into complete 40-byte packets,
 * resynchronising on the 01 03 20 header if a chunk is ever dropped. */
static void on_notify_data(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        const uint8_t b = data[i];

        /* Validate the header as it is being collected, so a mid-stream
         * resync can't stitch the tail of one packet onto the head of the
         * next and parse it as real data. */
        if (s_buf_len == 0 && b != HUGHES_WD_HDR0) {
            continue;
        }
        if ((s_buf_len == 1 && b != HUGHES_WD_HDR1) ||
            (s_buf_len == 2 && b != HUGHES_WD_HDR2)) {
            s_buf_len = 0;
            if (b == HUGHES_WD_HDR0) {
                s_buf[s_buf_len++] = b;
            }
            continue;
        }

        s_buf[s_buf_len++] = b;
        if (s_buf_len < HUGHES_WD_PACKET_LEN) {
            continue;
        }

        if (s_raw_logged < WD_RAW_LOG_PACKETS) {
            s_raw_logged++;
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, s_buf, HUGHES_WD_PACKET_LEN, ESP_LOG_INFO);
        }

        hughes_wd_reading_t r;
        if (hughes_wd_parse_packet(s_buf, HUGHES_WD_PACKET_LEN, &r)) {
            store_reading(&r);
            if (s_raw_logged <= WD_RAW_LOG_PACKETS) {
                ESP_LOGI(TAG, "L%u %.1fV %.1fA %.0fW %.2fHz %s",
                         (unsigned)r.line, (double)r.voltage_v, (double)r.current_a,
                         (double)r.power_w, (double)r.frequency_hz,
                         hughes_wd_error_str(r.error_code));
            }
        }
        s_buf_len = 0;
    }
}

/* ---------------------------------------------------------- discovery --- */

/*
 * Two callbacks into ble_host's shared scanner rather than a GAP event
 * handler of our own: `wd_scan_match` says which advertised name we want,
 * and `wd_scan_found` is called back once the scan has actually STOPPED,
 * which is what makes opening a connection from inside it safe.
 */

static bool wd_scan_match(const char *name, void *ctx)
{
    (void)ctx;
    if (s_state != WD_SCANNING || !hughes_wd_name_matches(name)) {
        return false;
    }
    ESP_LOGI(TAG, "found Watchdog advertising as '%s'", name);
    return true;
}

static void wd_scan_found(const esp_bd_addr_t bda, esp_ble_addr_type_t addr_type, void *ctx)
{
    (void)ctx;
    if (s_state != WD_SCANNING) {
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

static void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                esp_ble_gattc_cb_param_t *p)
{
    if (event == ESP_GATTC_REG_EVT) {
        /* Registration events fan out to every client on the node, so claim
         * only our own app_id: without this check the Watchdog would adopt
         * whichever gattc_if the batteries registered last and quietly stop
         * seeing its own events. */
        if (p->reg.app_id != WD_APP_ID) {
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
     * ⚠️ Bluedroid delivers connection events to EVERY registered GATTC app,
     * not just the one that opened the connection -- its documented
     * "virtual connection" behaviour. With four other apps on this node
     * (three battery packs and the solar controller), an unfiltered handler
     * adopts their connections as its own.
     *
     * This was observed on the bench: a battery pack's connect attempt
     * timing out made this client log "disconnected -- rescanning" and drop
     * out of WD_READY, so a perfectly healthy Watchdog would have read as
     * unhealthy and started competing for the shared scan. Every event
     * carrying a peer address is therefore checked against ours, exactly as
     * jbd_bms_client.c already does.
     */
    switch (event) {
    case ESP_GATTC_OPEN_EVT:
        if (memcmp(p->open.remote_bda, s_bda, sizeof(esp_bd_addr_t)) != 0) {
            return;
        }
        if (p->open.status != ESP_GATT_OK) {
            ESP_LOGW(TAG, "open failed, status %d -- rescanning", p->open.status);
            start_scan();
        }
        break;

    case ESP_GATTC_CONNECT_EVT:
        if (memcmp(p->connect.remote_bda, s_bda, sizeof(esp_bd_addr_t)) != 0) {
            return;
        }
        s_conn_id = p->connect.conn_id;
        s_svc_start = s_svc_end = s_notify_handle = 0;
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
        esp_ble_gattc_search_service(gattc_if, s_conn_id, (esp_bt_uuid_t *)&k_svc_uuid);
        break;

    case ESP_GATTC_SEARCH_RES_EVT:
        if (p->search_res.conn_id != s_conn_id) {
            return;
        }
        if (p->search_res.srvc_id.uuid.len == ESP_UUID_LEN_16 &&
            p->search_res.srvc_id.uuid.uuid.uuid16 == HUGHES_WD_SERVICE_UUID) {
            s_svc_start = p->search_res.start_handle;
            s_svc_end = p->search_res.end_handle;
        }
        break;

    case ESP_GATTC_SEARCH_CMPL_EVT: {
        if (p->search_cmpl.conn_id != s_conn_id) {
            return;
        }
        if (p->search_cmpl.status != ESP_GATT_OK || s_svc_start == 0) {
            ESP_LOGW(TAG, "service 0x%04X not found -- is this a Gen 2 unit? closing",
                     HUGHES_WD_SERVICE_UUID);
            esp_ble_gattc_close(gattc_if, s_conn_id);
            break;
        }
        esp_gattc_char_elem_t chars[WD_ATTR_RESULT_MAX];
        uint16_t count = WD_ATTR_RESULT_MAX;
        if (esp_ble_gattc_get_char_by_uuid(gattc_if, s_conn_id, s_svc_start, s_svc_end,
                                           k_notify_uuid, chars, &count) != ESP_GATT_OK ||
            count == 0) {
            ESP_LOGW(TAG, "notify characteristic 0x%04X not found -- closing",
                     HUGHES_WD_NOTIFY_UUID);
            esp_ble_gattc_close(gattc_if, s_conn_id);
            break;
        }
        s_notify_handle = chars[0].char_handle;
        esp_ble_gattc_register_for_notify(gattc_if, s_bda, s_notify_handle);
        break;
    }

    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
        if (p->reg_for_notify.status != ESP_GATT_OK) {
            ESP_LOGW(TAG, "notify registration failed, status %d", p->reg_for_notify.status);
            break;
        }
        esp_gattc_descr_elem_t descrs[WD_ATTR_RESULT_MAX];
        uint16_t count = WD_ATTR_RESULT_MAX;
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
        s_state = WD_READY;
        ESP_LOGI(TAG, "subscribed -- streaming telemetry from %s", s_peer_str);
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
        ESP_LOGW(TAG, "disconnected, reason 0x%02x -- rescanning", p->disconnect.reason);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (uint8_t i = 0; i < HUGHES_WD_MAX_LINES; i++) {
            s_reading_valid[i] = false;
        }
        xSemaphoreGive(s_lock);
        start_scan();
        break;

    default:
        break;
    }
}

/* ----------------------------------------------------------------- API -- */

esp_err_t hughes_wd_client_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Bluedroid is single-tenant for stack bring-up, for the one GAP and
     * one GATTC callback slot, and for the GAP scan -- and this node also
     * runs the battery and solar clients. ble_host owns all of it and fans
     * events out; registering or scanning directly here would silently
     * unhook (or cancel) whichever client got there first. */
    ESP_ERROR_CHECK(ble_host_add_gattc_observer(gattc_event_handler));

    s_scan_handle = ble_host_scan_add_matcher(wd_scan_match, wd_scan_found, NULL);
    if (s_scan_handle < 0) {
        ESP_LOGE(TAG, "no free scan matcher slot");
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(ble_host_start());
    ESP_ERROR_CHECK(esp_ble_gattc_app_register(WD_APP_ID));

    /* Ask for the first scan here rather than from the GATTC registration
     * event, which is where it used to live: ble_host runs one scan for
     * everybody, so there is nothing left to serialise against. */
    start_scan();

    ESP_LOGI(TAG, "Power Watchdog client started (scanning for a Gen 1 unit)");
    return ESP_OK;
}

bool hughes_wd_get_reading(uint8_t line, hughes_wd_reading_t *out)
{
    if (line < 1 || line > HUGHES_WD_MAX_LINES || out == NULL || s_lock == NULL) {
        return false;
    }
    const uint8_t idx = line - 1;
    bool ok = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_reading_valid[idx] &&
        (xTaskGetTickCount() - s_reading_tick[idx]) < pdMS_TO_TICKS(WD_STALE_WINDOW_MS)) {
        *out = s_reading[idx];
        ok = true;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

uint8_t hughes_wd_line_count(void)
{
    uint8_t n = 0;
    hughes_wd_reading_t r;
    for (uint8_t line = 1; line <= HUGHES_WD_MAX_LINES; line++) {
        if (hughes_wd_get_reading(line, &r)) {
            n++;
        }
    }
    return n;
}

bool hughes_wd_healthy(void)
{
    return s_state == WD_READY && hughes_wd_line_count() > 0;
}

const char *hughes_wd_peer_str(void)
{
    return s_peer_str;
}
