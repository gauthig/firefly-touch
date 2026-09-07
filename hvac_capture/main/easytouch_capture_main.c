/*
 * easytouch_capture — read-only bench capture tool for the Micro-Air
 * EasyTouch RV thermostat BLE protocol (issue #70).
 *
 * Scans for "EasyTouch <serial>", connects, authenticates with the
 * Micro-Air account password (see Kconfig -- this is the ONLY credential
 * the protocol uses; there is no separate BLE pairing, see
 * docs/EASYTOUCH-THERMOSTAT.md section 2), then polls "Get Status" and
 * logs the raw JSON plus a decoded per-zone summary to the console.
 *
 * Deliberately sends NO "Change" command, ever -- this tool cannot
 * accidentally toggle the A/C, heat, or Aqua-Hot while capturing. Drive
 * every mode/fan change from the physical touchscreen and watch this log
 * to fill in docs/EASYTOUCH-THERMOSTAT.md section 3.3's unknown mode
 * numbers (bench plan, section 8).
 *
 * Also subscribes to the status characteristic's notify (bench plan step
 * 6): if the thermostat ever pushes unsolicited, that shows up here as
 * "NOTIFY" lines instead of only the polled "STATUS" lines.
 *
 * Modeled on components/jbd_bms/jbd_bms_client.c and
 * components/renogy_solar/renogy_solar_client.c's connect/discover/
 * scan-by-name patterns, simplified to one fixed connection instead of a
 * slot table since there is exactly one thermostat.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gatt_defs.h"
#include "esp_gattc_api.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "ble_host.h"

static const char *TAG = "easytouch_capture";

#define ET_APP_ID 40 /* not in ble_host.h's shared list yet -- single-purpose bench tool */

#define ET_SVC_UUID    0x00FF
#define ET_CHR_AUTH    0xDD01
#define ET_CHR_CMD     0xEE01
#define ET_CHR_STATUS  0xFF01

#define ET_ADDR_TYPE   BLE_ADDR_TYPE_PUBLIC
#define ET_LOCAL_MTU   500
#define ET_STATUS_BUF_LEN 1024
#define ET_MAX_CHAR_RESULTS 2

typedef enum {
    ET_DISCONNECTED = 0,
    ET_CONNECTING,
    ET_DISCOVERING,
    ET_WRITING_AUTH,
    ET_READING_AUTH,
    ET_SUBSCRIBING,
    ET_READY,          /* authenticated, subscribed, polling on a timer */
    ET_WRITING_STATUS_REQ,
    ET_READING_STATUS,
} et_state_t;

static esp_gatt_if_t s_gattc_if = ESP_GATT_IF_NONE;
static esp_bd_addr_t s_bda;
static esp_ble_addr_type_t s_bda_type;
static uint16_t s_conn_id;
static et_state_t s_state = ET_DISCONNECTED;
static int s_scan_handle = -1;
static bool s_authenticated = false;

static uint16_t s_svc_start, s_svc_end;
static uint16_t s_h_auth, s_h_cmd, s_h_status;

static esp_timer_handle_t s_poll_timer;

static const esp_bt_uuid_t k_svc_uuid = {
    .len = ESP_UUID_LEN_16, .uuid = { .uuid16 = ET_SVC_UUID },
};
static const esp_bt_uuid_t k_auth_uuid = {
    .len = ESP_UUID_LEN_16, .uuid = { .uuid16 = ET_CHR_AUTH },
};
static const esp_bt_uuid_t k_cmd_uuid = {
    .len = ESP_UUID_LEN_16, .uuid = { .uuid16 = ET_CHR_CMD },
};
static const esp_bt_uuid_t k_status_uuid = {
    .len = ESP_UUID_LEN_16, .uuid = { .uuid16 = ET_CHR_STATUS },
};
static const esp_bt_uuid_t k_cccd_uuid = {
    .len = ESP_UUID_LEN_16, .uuid = { .uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG },
};

static void start_scan(void);
static void poll_timer_cb(void *arg);

/* ------------------------------------------------------------- decode -- */

/* Tiny scanner for exactly the two fields this bench tool needs out of the
 * status JSON -- not a general parser. Logs the whole raw payload too, so
 * nothing is lost if this misses a field the real parser will want later. */
static void log_decoded_status(const char *json, size_t len)
{
    ESP_LOGI(TAG, "STATUS raw (%d bytes): %.*s", (int)len, (int)len, json);

    const char *zsts = strstr(json, "\"Z_sts\"");
    if (!zsts) {
        ESP_LOGW(TAG, "  (no Z_sts in this payload -- truncated read? check MTU)");
        return;
    }
    const char *p = zsts;
    while ((p = strstr(p, "\"")) != NULL) {
        /* look for a zone key like "0":[ ... ] */
        int zone;
        int consumed = 0;
        if (sscanf(p, "\"%d\":[%n", &zone, &consumed) == 1 && consumed > 0) {
            const char *arr = p + consumed;
            int vals[16];
            int n = 0;
            const char *q = arr;
            while (n < 16 && *q && *q != ']') {
                int v, adv = 0;
                if (sscanf(q, "%d%n", &v, &adv) == 1) {
                    vals[n++] = v;
                    q += adv;
                    while (*q == ',' || *q == ' ') {
                        q++;
                    }
                } else {
                    break;
                }
            }
            if (n >= 16) {
                ESP_LOGI(TAG,
                         "  zone %d: inside=%dF mode=%d current=%d "
                         "cool_sp=%d heat_sp=%d dry_sp=%d auto=%d/%d "
                         "fan[only/cool/auto/heat]=%d/%d/%d/%d "
                         "unk[5,8,13,14]=%d,%d,%d,%d",
                         zone, vals[12], vals[10], vals[15],
                         vals[2], vals[3], vals[4], vals[0], vals[1],
                         vals[6], vals[7], vals[9], vals[11],
                         vals[5], vals[8], vals[13], vals[14]);
            } else {
                ESP_LOGW(TAG, "  zone %d: only %d of 16 elements -- short array", zone, n);
            }
            p = q;
        } else {
            p++;
        }
        if (p >= json + len) {
            break;
        }
    }
}

/* ------------------------------------------------------------- actions - */

static void request_status(void)
{
    const char *email = CONFIG_FIREFLY_EASYTOUCH_EMAIL;
    char msg[160];
    int msg_len;
    if (email[0] != '\0') {
        /* Deliberate opt-in only (bench step 2's A/B test) -- there is no
         * real clock here, so TM is uptime seconds, NOT a real epoch. Never
         * do this in the real gateway; see docs/EASYTOUCH-THERMOSTAT.md
         * section 3.3 on why TM is normally omitted entirely. */
        int64_t fake_tm = esp_timer_get_time() / 1000000;
        ESP_LOGW(TAG, "sending EM/TM per Kconfig override (TM=%" PRId64
                 " is uptime, NOT a real timestamp -- bench-only)", fake_tm);
        msg_len = snprintf(msg, sizeof(msg),
                           "{\"Type\":\"Get Status\",\"Zone\":0,\"EM\":\"%s\",\"TM\":%" PRId64 "}",
                           email, fake_tm);
    } else {
        msg_len = snprintf(msg, sizeof(msg), "{\"Type\":\"Get Status\",\"Zone\":0}");
    }
    ESP_LOGI(TAG, "-> %s", msg);
    s_state = ET_WRITING_STATUS_REQ;
    esp_ble_gattc_write_char(s_gattc_if, s_conn_id, s_h_cmd,
                             (uint16_t)msg_len, (uint8_t *)msg,
                             ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
}

static void poll_timer_cb(void *arg)
{
    (void)arg;
    if (s_state == ET_READY) {
        request_status();
    }
}

static void reset_to_scan(const char *why)
{
    ESP_LOGW(TAG, "resetting: %s", why);
    if (s_poll_timer) {
        esp_timer_stop(s_poll_timer);
    }
    s_state = ET_DISCONNECTED;
    s_authenticated = false;
    s_svc_start = s_svc_end = s_h_auth = s_h_cmd = s_h_status = 0;
    ble_host_scan_want(s_scan_handle, true);
}

static void try_connect(void)
{
    ESP_LOGI(TAG, "connecting...");
    s_state = ET_CONNECTING;
#if defined(CONFIG_BT_BLE_50_FEATURES_SUPPORTED)
    esp_ble_gatt_creat_conn_params_t conn_params = { 0 };
    memcpy(conn_params.remote_bda, s_bda, sizeof(esp_bd_addr_t));
    conn_params.remote_addr_type = ET_ADDR_TYPE;
    conn_params.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
    conn_params.is_direct = true;
    conn_params.is_aux = false;
    conn_params.phy_mask = 0x0;
    esp_err_t err = esp_ble_gattc_enh_open(s_gattc_if, &conn_params);
#else
    esp_err_t err = esp_ble_gattc_open(s_gattc_if, s_bda, ET_ADDR_TYPE, true);
#endif
    if (err != ESP_OK) {
        reset_to_scan("gattc open failed");
    }
}

/* ------------------------------------------------------------ scan ----- */

static bool scan_match(const char *name, void *ctx)
{
    (void)ctx;
    if (s_state != ET_DISCONNECTED) {
        return false;
    }
    const char *want = CONFIG_FIREFLY_EASYTOUCH_NAME_PREFIX;
    if (strncmp(name, want, strlen(want)) != 0) {
        return false;
    }
    ESP_LOGI(TAG, "found thermostat advertising as '%s'", name);
    return true;
}

static void scan_found(const esp_bd_addr_t bda, esp_ble_addr_type_t addr_type, void *ctx)
{
    (void)ctx;
    memcpy(s_bda, bda, sizeof(esp_bd_addr_t));
    s_bda_type = addr_type;
    ESP_LOGI(TAG, "peer %02X:%02X:%02X:%02X:%02X:%02X (addr type %d)",
             s_bda[0], s_bda[1], s_bda[2], s_bda[3], s_bda[4], s_bda[5], s_bda_type);
    try_connect();
}

static void start_scan(void)
{
    ble_host_scan_want(s_scan_handle, true);
}

/* --------------------------------------------------------- gattc event - */

static void on_gattc_event(esp_gattc_cb_event_t event, esp_ble_gattc_cb_param_t *p)
{
    switch (event) {
    case ESP_GATTC_OPEN_EVT:
        if (memcmp(p->open.remote_bda, s_bda, sizeof(esp_bd_addr_t)) != 0) {
            break;
        }
        if (p->open.status != ESP_GATT_OK) {
            reset_to_scan("open failed");
            break;
        }
        esp_ble_gatt_set_local_mtu(ET_LOCAL_MTU);
        break;

    case ESP_GATTC_CONNECT_EVT:
        if (memcmp(p->connect.remote_bda, s_bda, sizeof(esp_bd_addr_t)) != 0) {
            break;
        }
        s_conn_id = p->connect.conn_id;
        s_state = ET_DISCOVERING;
        esp_ble_gattc_send_mtu_req(s_gattc_if, s_conn_id);
        break;

    case ESP_GATTC_CFG_MTU_EVT:
        ESP_LOGI(TAG, "MTU negotiated: %d", p->cfg_mtu.mtu);
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
            reset_to_scan("EasyTouch service (0x00FF) not found");
            esp_ble_gattc_close(s_gattc_if, s_conn_id);
            break;
        }
        esp_gattc_char_elem_t chars[ET_MAX_CHAR_RESULTS];
        uint16_t count;

        count = ET_MAX_CHAR_RESULTS;
        if (esp_ble_gattc_get_char_by_uuid(s_gattc_if, s_conn_id, s_svc_start, s_svc_end,
                                           k_auth_uuid, chars, &count) == ESP_GATT_OK && count > 0) {
            s_h_auth = chars[0].char_handle;
        }
        count = ET_MAX_CHAR_RESULTS;
        if (esp_ble_gattc_get_char_by_uuid(s_gattc_if, s_conn_id, s_svc_start, s_svc_end,
                                           k_cmd_uuid, chars, &count) == ESP_GATT_OK && count > 0) {
            s_h_cmd = chars[0].char_handle;
        }
        count = ET_MAX_CHAR_RESULTS;
        if (esp_ble_gattc_get_char_by_uuid(s_gattc_if, s_conn_id, s_svc_start, s_svc_end,
                                           k_status_uuid, chars, &count) == ESP_GATT_OK && count > 0) {
            s_h_status = chars[0].char_handle;
        }

        if (s_h_auth == 0 || s_h_cmd == 0 || s_h_status == 0) {
            reset_to_scan("one or more characteristics (DD01/EE01/FF01) not found");
            esp_ble_gattc_close(s_gattc_if, s_conn_id);
            break;
        }
        ESP_LOGI(TAG, "characteristics found, authenticating...");
        s_state = ET_WRITING_AUTH;
        {
            const char *pw = CONFIG_FIREFLY_EASYTOUCH_PASSWORD;
            if (pw[0] == '\0') {
                ESP_LOGE(TAG, "FIREFLY_EASYTOUCH_PASSWORD is empty -- set it with "
                              "idf.py -C hvac_capture -B hvac_capture/build menuconfig");
                esp_ble_gattc_close(s_gattc_if, s_conn_id);
                break;
            }
            esp_ble_gattc_write_char(s_gattc_if, s_conn_id, s_h_auth,
                                     (uint16_t)strlen(pw), (uint8_t *)pw,
                                     ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
        }
        break;
    }

    case ESP_GATTC_WRITE_CHAR_EVT:
        if (p->write.status != ESP_GATT_OK) {
            reset_to_scan("GATT write failed");
            esp_ble_gattc_close(s_gattc_if, s_conn_id);
            break;
        }
        if (s_state == ET_WRITING_AUTH && p->write.handle == s_h_auth) {
            s_state = ET_READING_AUTH;
            esp_ble_gattc_read_char(s_gattc_if, s_conn_id, s_h_auth, ESP_GATT_AUTH_REQ_NONE);
        } else if (s_state == ET_WRITING_STATUS_REQ && p->write.handle == s_h_cmd) {
            s_state = ET_READING_STATUS;
            /* The thermostat needs a moment to build the response; every
             * public implementation sleeps here rather than reading
             * immediately. */
            vTaskDelay(pdMS_TO_TICKS(400));
            esp_ble_gattc_read_char(s_gattc_if, s_conn_id, s_h_status, ESP_GATT_AUTH_REQ_NONE);
        }
        break;

    case ESP_GATTC_READ_CHAR_EVT:
        if (p->read.status != ESP_GATT_OK) {
            reset_to_scan("GATT read failed");
            esp_ble_gattc_close(s_gattc_if, s_conn_id);
            break;
        }
        if (s_state == ET_READING_AUTH && p->read.handle == s_h_auth) {
            char reply[64];
            size_t n = p->read.value_len < sizeof(reply) - 1 ? p->read.value_len : sizeof(reply) - 1;
            memcpy(reply, p->read.value, n);
            reply[n] = '\0';
            ESP_LOGI(TAG, "auth readback: '%s'", reply);
            if (strncmp(reply, "Matched", 7) == 0) {
                s_authenticated = true;
                ESP_LOGI(TAG, "authenticated -- subscribing to status notify");
                s_state = ET_SUBSCRIBING;
                esp_ble_gattc_register_for_notify(s_gattc_if, s_bda, s_h_status);
            } else {
                ESP_LOGE(TAG, "PASSWORD NOT MATCHED -- check FIREFLY_EASYTOUCH_PASSWORD "
                              "against the phone app's account password. Not retrying "
                              "in a loop; power-cycle the thermostat if repeated auth "
                              "failures are a concern.");
                esp_ble_gattc_close(s_gattc_if, s_conn_id);
            }
        } else if (s_state == ET_READING_STATUS && p->read.handle == s_h_status) {
            log_decoded_status((const char *)p->read.value, p->read.value_len);
            s_state = ET_READY;
        }
        break;

    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
        esp_gattc_descr_elem_t descrs[ET_MAX_CHAR_RESULTS];
        uint16_t count = ET_MAX_CHAR_RESULTS;
        if (p->reg_for_notify.status == ESP_GATT_OK &&
            esp_ble_gattc_get_descr_by_char_handle(s_gattc_if, s_conn_id, s_h_status,
                                                   k_cccd_uuid, descrs, &count) == ESP_GATT_OK &&
            count > 0) {
            uint8_t enable[2] = { 0x01, 0x00 };
            esp_ble_gattc_write_char_descr(s_gattc_if, s_conn_id, descrs[0].handle,
                                           sizeof(enable), enable,
                                           ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
            ESP_LOGI(TAG, "subscribed for notify on status characteristic "
                          "(bench step 6: watch for unsolicited pushes)");
        } else {
            ESP_LOGW(TAG, "no CCCD on status characteristic -- it will not notify, "
                          "polling only (this itself answers bench step 6)");
        }
        ESP_LOGI(TAG, "ready -- polling every %d ms. Requesting first status now.",
                 CONFIG_FIREFLY_EASYTOUCH_POLL_INTERVAL_MS);
        s_state = ET_READY;
        esp_timer_start_periodic(s_poll_timer, (uint64_t)CONFIG_FIREFLY_EASYTOUCH_POLL_INTERVAL_MS * 1000);
        request_status();
        break;
    }

    case ESP_GATTC_NOTIFY_EVT:
        ESP_LOGI(TAG, "*** NOTIFY (unsolicited push, not a poll response) ***");
        log_decoded_status((const char *)p->notify.value, p->notify.value_len);
        break;

    case ESP_GATTC_DISCONNECT_EVT:
        if (memcmp(p->disconnect.remote_bda, s_bda, sizeof(esp_bd_addr_t)) != 0) {
            break;
        }
        ESP_LOGW(TAG, "disconnected, reason 0x%02x%s", p->disconnect.reason,
                 s_authenticated
                     ? " (phone app connecting? this is expected -- will retry)"
                     : "");
        reset_to_scan("disconnect event");
        break;

    default:
        break;
    }
}

static void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                esp_ble_gattc_cb_param_t *param)
{
    if (event == ESP_GATTC_REG_EVT) {
        if (param->reg.app_id != ET_APP_ID) {
            return;
        }
        if (param->reg.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "app register failed, status %d", param->reg.status);
            return;
        }
        s_gattc_if = gattc_if;
        ESP_LOGI(TAG, "app registered, gattc_if %d -- scanning for '%s*'",
                 gattc_if, CONFIG_FIREFLY_EASYTOUCH_NAME_PREFIX);
        start_scan();
        return;
    }
    if (gattc_if != s_gattc_if) {
        return;
    }
    on_gattc_event(event, param);
}

void app_main(void)
{
    ESP_LOGI(TAG, "EasyTouch bench capture (issue #70) -- READ ONLY, sends no Change commands");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(ble_host_start());
    ESP_ERROR_CHECK(ble_host_add_gattc_observer(gattc_event_handler));

    s_scan_handle = ble_host_scan_add_matcher(scan_match, scan_found, NULL);
    if (s_scan_handle < 0) {
        ESP_LOGE(TAG, "no room for a scan matcher");
        return;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = poll_timer_cb,
        .name = "et_poll",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_poll_timer));

    ESP_ERROR_CHECK(esp_ble_gattc_app_register(ET_APP_ID));
}
