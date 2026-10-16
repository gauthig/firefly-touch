/*
 * See easytouch_client.h. Connect -> auth -> (send queued Changes) -> Get
 * Status -> read -> DISCONNECT, every CONFIG_FIREFLY_EASYTOUCH_POLL_INTERVAL_MS.
 *
 * Discovery: scan by advertised-name prefix once, pin the MAC, then
 * direct-connect by address thereafter (like renogy_solar_client). Fall back
 * to a re-scan after several failed direct connects.
 *
 * Modelled on components/jbd_bms/jbd_bms_client.c and the hvac_capture bench
 * tool that first proved this protocol on the coach (removed in issue #83;
 * its captures live on in docs/EASYTOUCH-THERMOSTAT.md and the host tests).
 */
#include "easytouch_client.h"

#include <string.h>

#include "ble_host.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gatt_defs.h"
#include "esp_gattc_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "easytouch_protocol.h"

static const char *TAG = "easytouch_client";

#define ET_SVC_UUID   0x00FF
#define ET_CHR_AUTH   0xDD01
#define ET_CHR_CMD    0xEE01
#define ET_CHR_STATUS 0xFF01

#define ET_ADDR_TYPE     BLE_ADDR_TYPE_PUBLIC
#define ET_LOCAL_MTU     500
#define ET_STATUS_BUF    1024
#define ET_CHAR_RESULTS  2

#define ET_TASK_STACK    4608
#define ET_TASK_PRIO     9
#define ET_TASK_PERIOD_MS 1000
#define ET_CONNECT_TIMEOUT_MS 15000   /* give up on a stuck connect attempt */
#define ET_RETRY_MS      5000         /* wait after a failed/short cycle */
#define ET_RESCAN_AFTER_FAILS 3       /* forget the pinned MAC after this many */
#define ET_STATUS_READ_DELAY_MS 400   /* thermostat builds the response; every
                                       * public client sleeps here before read */

/* Staleness window, derived from the poll interval so it always exceeds it
 * (same reasoning as JBD_HEALTHY_WINDOW_MS). */
#define ET_HEALTHY_WINDOW_MS (3 * CONFIG_FIREFLY_EASYTOUCH_POLL_INTERVAL_MS)

typedef enum {
    ET_IDLE = 0,        /* disconnected; waiting for next poll tick */
    ET_CONNECTING,
    ET_DISCOVERING,
    ET_AUTH_WRITE,
    ET_AUTH_READ,
    ET_CMD_WRITE,       /* draining the pending-change queue */
    ET_STATUS_WRITE,
    ET_STATUS_READ,
} et_state_t;

static esp_gatt_if_t s_gattc_if = ESP_GATT_IF_NONE;
static et_state_t     s_state = ET_IDLE;

static esp_bd_addr_t  s_bda;
static bool           s_bda_valid;
static uint8_t        s_connect_fails;

static uint16_t s_conn_id;
static uint16_t s_svc_start, s_svc_end;
static uint16_t s_h_auth, s_h_cmd, s_h_status;

static int        s_scan_handle = -1;
static bool       s_scan_pending;   /* asked ble_host for a scan, awaiting found */
static TickType_t s_connect_deadline;
static TickType_t s_next_poll_tick;

static SemaphoreHandle_t s_lock;
static easytouch_status_t s_status;
static bool               s_status_valid;
static TickType_t         s_last_valid_tick;

/* One coalescing pending Change per zone. */
static easytouch_change_t s_pending[EASYTOUCH_MAX_ZONES];
static bool               s_pending_flag[EASYTOUCH_MAX_ZONES];
static uint8_t            s_cmd_zone;   /* zone currently being written in ET_CMD_WRITE */

static char s_status_buf[ET_STATUS_BUF];

static const esp_bt_uuid_t k_svc_uuid    = { .len = ESP_UUID_LEN_16, .uuid = { .uuid16 = ET_SVC_UUID } };
static const esp_bt_uuid_t k_auth_uuid   = { .len = ESP_UUID_LEN_16, .uuid = { .uuid16 = ET_CHR_AUTH } };
static const esp_bt_uuid_t k_cmd_uuid    = { .len = ESP_UUID_LEN_16, .uuid = { .uuid16 = ET_CHR_CMD } };
static const esp_bt_uuid_t k_status_uuid = { .len = ESP_UUID_LEN_16, .uuid = { .uuid16 = ET_CHR_STATUS } };

/* ------------------------------------------------------------ helpers --- */

static void go_idle_retry(const char *why, uint32_t wait_ms)
{
    if (why) {
        ESP_LOGW(TAG, "cycle ended: %s", why);
    }
    s_state = ET_IDLE;
    s_svc_start = s_svc_end = s_h_auth = s_h_cmd = s_h_status = 0;
    s_next_poll_tick = xTaskGetTickCount() + pdMS_TO_TICKS(wait_ms);
}

static void close_conn(void)
{
    if (s_gattc_if != ESP_GATT_IF_NONE) {
        esp_ble_gattc_close(s_gattc_if, s_conn_id);
    }
}

static void request_status(void)
{
    char msg[64];
    const size_t n = easytouch_build_get_status(msg, sizeof(msg));
    s_state = ET_STATUS_WRITE;
    esp_ble_gattc_write_char(s_gattc_if, s_conn_id, s_h_cmd, (uint16_t)n, (uint8_t *)msg,
                             ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
}

/*
 * Write the pending Change for the first zone at or after `from` that
 * actually produces a message; clear the flag for any zone that doesn't.
 * When none are left, fall through to the status request. The WRITE_CHAR_EVT
 * handler calls back here with `s_cmd_zone + 1` to continue the drain.
 */
static void drain_changes_from(uint8_t from)
{
    for (uint8_t z = from; z < EASYTOUCH_MAX_ZONES; z++) {
        if (!s_pending_flag[z]) {
            continue;
        }
        easytouch_change_t c;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        c = s_pending[z];
        xSemaphoreGive(s_lock);
        c.zone = z;

        char msg[256];
        const size_t n = easytouch_build_change(msg, sizeof(msg), &c);
        if (n == 0) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_pending_flag[z] = false;
            xSemaphoreGive(s_lock);
            continue;
        }
        ESP_LOGI(TAG, "-> %.*s", (int)n, msg);
        s_state = ET_CMD_WRITE;
        s_cmd_zone = z;
        esp_ble_gattc_write_char(s_gattc_if, s_conn_id, s_h_cmd, (uint16_t)n,
                                 (uint8_t *)msg, ESP_GATT_WRITE_TYPE_RSP,
                                 ESP_GATT_AUTH_REQ_NONE);
        return;
    }
    request_status();
}

/* ------------------------------------------------------------- connect -- */

static void try_connect(void)
{
    ESP_LOGI(TAG, "connecting to %02X:%02X:%02X:%02X:%02X:%02X",
             s_bda[0], s_bda[1], s_bda[2], s_bda[3], s_bda[4], s_bda[5]);
    s_state = ET_CONNECTING;
    s_connect_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ET_CONNECT_TIMEOUT_MS);

#if defined(CONFIG_BT_BLE_50_FEATURES_SUPPORTED)
    esp_ble_gatt_creat_conn_params_t cp = { 0 };
    memcpy(cp.remote_bda, s_bda, sizeof(esp_bd_addr_t));
    cp.remote_addr_type = ET_ADDR_TYPE;
    cp.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
    cp.is_direct = true;
    cp.is_aux = false;
    cp.phy_mask = 0x0;
    esp_err_t err = esp_ble_gattc_enh_open(s_gattc_if, &cp);
#else
    esp_err_t err = esp_ble_gattc_open(s_gattc_if, s_bda, ET_ADDR_TYPE, true);
#endif
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "gattc open failed: %s", esp_err_to_name(err));
        s_connect_fails++;
        go_idle_retry(NULL, ET_RETRY_MS);
    }
}

/* ------------------------------------------------------------- scan ----- */

static bool scan_match(const char *name, void *ctx)
{
    (void)ctx;
    if (s_state != ET_IDLE || !s_scan_pending) {
        return false;
    }
    if (!easytouch_name_matches(name)) {
        return false;
    }
    ESP_LOGI(TAG, "found thermostat advertising as '%s'", name);
    return true;
}

static void scan_found(const esp_bd_addr_t bda, esp_ble_addr_type_t addr_type, void *ctx)
{
    (void)ctx;
    (void)addr_type;
    memcpy(s_bda, bda, sizeof(esp_bd_addr_t));
    s_bda_valid = true;
    s_scan_pending = false;
    s_connect_fails = 0;
    try_connect();
}

/* ------------------------------------------------------- gattc events --- */

static void on_gattc_event(esp_gattc_cb_event_t event, esp_ble_gattc_cb_param_t *p)
{
    switch (event) {
    case ESP_GATTC_OPEN_EVT:
        if (memcmp(p->open.remote_bda, s_bda, sizeof(esp_bd_addr_t)) != 0) {
            break;
        }
        if (p->open.status != ESP_GATT_OK) {
            ESP_LOGW(TAG, "open failed, status %d", p->open.status);
            s_connect_fails++;
            go_idle_retry(NULL, ET_RETRY_MS);
            break;
        }
        esp_ble_gatt_set_local_mtu(ET_LOCAL_MTU);
        break;

    case ESP_GATTC_CONNECT_EVT:
        if (memcmp(p->connect.remote_bda, s_bda, sizeof(esp_bd_addr_t)) != 0) {
            break;   /* Bluedroid virtual-connection fan-out for another app */
        }
        s_conn_id = p->connect.conn_id;
        s_state = ET_DISCOVERING;
        esp_ble_gattc_send_mtu_req(s_gattc_if, s_conn_id);
        break;

    case ESP_GATTC_CFG_MTU_EVT:
        esp_ble_gattc_search_service(s_gattc_if, s_conn_id, (esp_bt_uuid_t *)&k_svc_uuid);
        break;

    case ESP_GATTC_SEARCH_RES_EVT:
        if (p->search_res.srvc_id.uuid.len == ESP_UUID_LEN_16 &&
            p->search_res.srvc_id.uuid.uuid.uuid16 == ET_SVC_UUID) {
            s_svc_start = p->search_res.start_handle;
            s_svc_end = p->search_res.end_handle;
        }
        break;

    case ESP_GATTC_SEARCH_CMPL_EVT: {
        if (p->search_cmpl.status != ESP_GATT_OK || s_svc_start == 0) {
            close_conn();
            go_idle_retry("EasyTouch service 0x00FF not found", ET_RETRY_MS);
            break;
        }
        esp_gattc_char_elem_t chars[ET_CHAR_RESULTS];
        uint16_t count;

        count = ET_CHAR_RESULTS;
        if (esp_ble_gattc_get_char_by_uuid(s_gattc_if, s_conn_id, s_svc_start, s_svc_end,
                                           k_auth_uuid, chars, &count) == ESP_GATT_OK && count > 0) {
            s_h_auth = chars[0].char_handle;
        }
        count = ET_CHAR_RESULTS;
        if (esp_ble_gattc_get_char_by_uuid(s_gattc_if, s_conn_id, s_svc_start, s_svc_end,
                                           k_cmd_uuid, chars, &count) == ESP_GATT_OK && count > 0) {
            s_h_cmd = chars[0].char_handle;
        }
        count = ET_CHAR_RESULTS;
        if (esp_ble_gattc_get_char_by_uuid(s_gattc_if, s_conn_id, s_svc_start, s_svc_end,
                                           k_status_uuid, chars, &count) == ESP_GATT_OK && count > 0) {
            s_h_status = chars[0].char_handle;
        }
        if (s_h_auth == 0 || s_h_cmd == 0 || s_h_status == 0) {
            close_conn();
            go_idle_retry("DD01/EE01/FF01 not all present", ET_RETRY_MS);
            break;
        }

        const char *pw = CONFIG_FIREFLY_EASYTOUCH_PASSWORD;
        if (pw[0] == '\0') {
            close_conn();
            go_idle_retry("FIREFLY_EASYTOUCH_PASSWORD is empty", 60000);
            break;
        }
        s_state = ET_AUTH_WRITE;
        esp_ble_gattc_write_char(s_gattc_if, s_conn_id, s_h_auth, (uint16_t)strlen(pw),
                                 (uint8_t *)pw, ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
        break;
    }

    case ESP_GATTC_WRITE_CHAR_EVT:
        if (p->write.status != ESP_GATT_OK) {
            close_conn();
            go_idle_retry("GATT write failed", ET_RETRY_MS);
            break;
        }
        if (s_state == ET_AUTH_WRITE && p->write.handle == s_h_auth) {
            s_state = ET_AUTH_READ;
            esp_ble_gattc_read_char(s_gattc_if, s_conn_id, s_h_auth, ESP_GATT_AUTH_REQ_NONE);
        } else if (s_state == ET_CMD_WRITE && p->write.handle == s_h_cmd) {
            /* This change is on the wire -- clear its pending flag and move on. */
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_pending_flag[s_cmd_zone] = false;
            xSemaphoreGive(s_lock);
            drain_changes_from(s_cmd_zone + 1);
        } else if (s_state == ET_STATUS_WRITE && p->write.handle == s_h_cmd) {
            s_state = ET_STATUS_READ;
            vTaskDelay(pdMS_TO_TICKS(ET_STATUS_READ_DELAY_MS));
            esp_ble_gattc_read_char(s_gattc_if, s_conn_id, s_h_status, ESP_GATT_AUTH_REQ_NONE);
        }
        break;

    case ESP_GATTC_READ_CHAR_EVT:
        if (p->read.status != ESP_GATT_OK) {
            close_conn();
            go_idle_retry("GATT read failed", ET_RETRY_MS);
            break;
        }
        if (s_state == ET_AUTH_READ && p->read.handle == s_h_auth) {
            char reply[48];
            size_t n = p->read.value_len < sizeof(reply) - 1 ? p->read.value_len
                                                             : sizeof(reply) - 1;
            memcpy(reply, p->read.value, n);
            reply[n] = '\0';
            if (strncmp(reply, EASYTOUCH_AUTH_OK_PREFIX, strlen(EASYTOUCH_AUTH_OK_PREFIX)) == 0) {
                s_connect_fails = 0;
                drain_changes_from(0);
            } else {
                ESP_LOGE(TAG, "auth NOT matched: '%s' -- check FIREFLY_EASYTOUCH_PASSWORD", reply);
                close_conn();
                /* Back off hard: a thermostat hammered with bad auth has been
                 * reported to need a power cycle. */
                go_idle_retry(NULL, 60000);
            }
        } else if (s_state == ET_STATUS_READ && p->read.handle == s_h_status) {
            size_t n = p->read.value_len < sizeof(s_status_buf) ? p->read.value_len
                                                               : sizeof(s_status_buf);
            memcpy(s_status_buf, p->read.value, n);
            easytouch_status_t parsed;
            if (easytouch_parse_status(s_status_buf, n, &parsed)) {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_status = parsed;
                s_status_valid = true;
                s_last_valid_tick = xTaskGetTickCount();
                xSemaphoreGive(s_lock);
                ESP_LOGI(TAG, "status: %u zone(s), SN %s%s", parsed.zone_count,
                         parsed.serial[0] ? parsed.serial : "?",
                         parsed.system_active ? ", active" : "");
            } else {
                /* Log the head of the payload sanitised (control chars -> '.')
                 * so a format change can be diagnosed from a serial capture.
                 * A short read here means the MTU request did not take. */
                char head[97];
                size_t hn = n < sizeof(head) - 1 ? n : sizeof(head) - 1;
                for (size_t j = 0; j < hn; j++) {
                    char c = s_status_buf[j];
                    head[j] = (c >= 0x20 && c < 0x7f) ? c : '.';
                }
                head[hn] = '\0';
                ESP_LOGW(TAG, "status parse failed (%d bytes): %s", (int)n, head);
            }
            /* Poll-and-release: drop the link so the phone app can get in.
             * Set the next poll tick HERE, not only in DISCONNECT_EVT --
             * the task runs on its own clock and could otherwise see
             * ET_IDLE with a stale (past) tick and start a fresh connect
             * before the disconnect has settled. */
            s_next_poll_tick = xTaskGetTickCount() +
                               pdMS_TO_TICKS(CONFIG_FIREFLY_EASYTOUCH_POLL_INTERVAL_MS);
            s_state = ET_IDLE;
            close_conn();
        }
        break;

    case ESP_GATTC_DISCONNECT_EVT:
        if (memcmp(p->disconnect.remote_bda, s_bda, sizeof(esp_bd_addr_t)) != 0) {
            break;
        }
        s_svc_start = s_svc_end = s_h_auth = s_h_cmd = s_h_status = 0;
        if (s_state == ET_IDLE) {
            /* Clean release after a good cycle -- the read handler already
             * scheduled the next poll. Nothing to do. */
        } else {
            ESP_LOGW(TAG, "unexpected disconnect, reason 0x%02x", p->disconnect.reason);
            s_connect_fails++;
            go_idle_retry(NULL, ET_RETRY_MS);
        }
        break;

    default:
        break;
    }
}

static void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                esp_ble_gattc_cb_param_t *param)
{
    if (event == ESP_GATTC_REG_EVT) {
        if (param->reg.app_id != BLE_HOST_APP_ID_EASYTOUCH) {
            return;   /* another client's registration */
        }
        if (param->reg.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "app register failed, status %d", param->reg.status);
            return;
        }
        s_gattc_if = gattc_if;
        ESP_LOGI(TAG, "app registered, gattc_if %d", gattc_if);
        return;
    }
    if (gattc_if != s_gattc_if || s_gattc_if == ESP_GATT_IF_NONE) {
        return;
    }
    on_gattc_event(event, param);
}

/* ------------------------------------------------------------- task ----- */

static void easytouch_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(ET_TASK_PERIOD_MS));
        const TickType_t now = xTaskGetTickCount();

        switch (s_state) {
        case ET_IDLE:
            if ((int32_t)(now - s_next_poll_tick) < 0) {
                break;
            }
            if (s_bda_valid && s_connect_fails < ET_RESCAN_AFTER_FAILS) {
                try_connect();
            } else if (!s_scan_pending) {
                s_bda_valid = false;
                s_scan_pending = true;
                ble_host_scan_want(s_scan_handle, true);
                ESP_LOGI(TAG, "scanning for '%s'", CONFIG_FIREFLY_EASYTOUCH_NAME_PREFIX);
            }
            break;

        case ET_CONNECTING:
            if ((int32_t)(now - s_connect_deadline) >= 0) {
                ESP_LOGW(TAG, "connect timed out");
                close_conn();
                s_connect_fails++;
                go_idle_retry(NULL, ET_RETRY_MS);
            }
            break;

        default:
            /* A cycle stuck mid-handshake for far too long: force it round. */
            if ((int32_t)(now - s_connect_deadline) >= 0) {
                close_conn();
                go_idle_retry("handshake stalled", ET_RETRY_MS);
            }
            break;
        }
    }
}

/* ------------------------------------------------------------ lifecycle - */

esp_err_t easytouch_client_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (CONFIG_FIREFLY_EASYTOUCH_PASSWORD[0] == '\0') {
        ESP_LOGW(TAG, "FIREFLY_EASYTOUCH_PASSWORD empty -- client will idle. "
                      "Set it in this build dir's sdkconfig.");
    }

    ESP_ERROR_CHECK(ble_host_add_gattc_observer(gattc_event_handler));
    ESP_ERROR_CHECK(ble_host_start());

    s_scan_handle = ble_host_scan_add_matcher(scan_match, scan_found, NULL);
    if (s_scan_handle < 0) {
        ESP_LOGE(TAG, "no room for a scan matcher");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t reg = esp_ble_gattc_app_register(BLE_HOST_APP_ID_EASYTOUCH);
    if (reg != ESP_OK) {
        ESP_LOGW(TAG, "app register call failed: %s", esp_err_to_name(reg));
    }

    s_next_poll_tick = xTaskGetTickCount();
    if (xTaskCreate(easytouch_task, "easytouch", ET_TASK_STACK, NULL,
                    ET_TASK_PRIO, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "EasyTouch client started (poll every %d ms, release between)",
             CONFIG_FIREFLY_EASYTOUCH_POLL_INTERVAL_MS);
    return ESP_OK;
}

bool easytouch_client_get_status(easytouch_status_t *out)
{
    if (out == NULL || s_lock == NULL) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool ok = s_status_valid;
    if (ok) {
        *out = s_status;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

bool easytouch_client_healthy(void)
{
    if (s_lock == NULL || !s_status_valid) {
        return false;
    }
    return (xTaskGetTickCount() - s_last_valid_tick) < pdMS_TO_TICKS(ET_HEALTHY_WINDOW_MS);
}

bool easytouch_client_submit_change(const easytouch_change_t *change)
{
    if (change == NULL || change->zone >= EASYTOUCH_MAX_ZONES || s_lock == NULL) {
        return false;
    }
    const uint8_t z = change->zone;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_pending_flag[z]) {
        s_pending[z] = *change;
    } else {
        /* Merge field-wise: OR the set_* flags, overwrite the values. */
        easytouch_change_t *d = &s_pending[z];
#define ET_MERGE(setf, valf) do { if (change->setf) { d->setf = true; d->valf = change->valf; } } while (0)
        ET_MERGE(set_power, power);
        ET_MERGE(set_mode, mode);
        ET_MERGE(set_cool_sp, cool_sp);
        ET_MERGE(set_heat_sp, heat_sp);
        ET_MERGE(set_dry_sp, dry_sp);
        ET_MERGE(set_auto_heat_sp, auto_heat_sp);
        ET_MERGE(set_auto_cool_sp, auto_cool_sp);
        ET_MERGE(set_cool_fan, cool_fan);
        ET_MERGE(set_heat_fan, heat_fan);
        ET_MERGE(set_auto_fan, auto_fan);
        ET_MERGE(set_fan_only, fan_only);
#undef ET_MERGE
    }
    s_pending[z].zone = z;
    s_pending_flag[z] = true;
    /* Pull the next poll forward so the change goes out promptly -- but not
     * to *now*: reconnecting within ~1 s of the previous disconnect gets
     * "connection failed to be established" (HCI 0x3e) and a retry, which is
     * slower overall. A short settle gap plus the UI's optimistic display
     * makes this feel instant anyway. Never push the tick later than it
     * already is (e.g. a change submitted mid-cycle). */
    const TickType_t soon = xTaskGetTickCount() + pdMS_TO_TICKS(1200);
    if ((int32_t)(soon - s_next_poll_tick) < 0) {
        s_next_poll_tick = soon;
    }
    xSemaphoreGive(s_lock);
    return true;
}
