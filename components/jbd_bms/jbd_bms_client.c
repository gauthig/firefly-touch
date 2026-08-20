/*
 * jbd_bms_client — Bluedroid GATT-client central role, up to
 * JBD_BMS_MAX_BATTERIES fixed peripherals. Each slot is its own GATTC
 * "app" (one esp_gattc_app_register() per battery, app_id == slot index)
 * connected directly by known BLE address (no scanning -- the MACs are
 * fixed via Kconfig, same v1 scoping as components/espnow_link's single
 * fixed ESP-NOW peer). Once connected: discover service (0xFF00) ->
 * discover characteristics (notify 0xFF01, write 0xFF02) -> register for
 * notify -> discover + write the notify characteristic's CCCD -> ready,
 * then periodic write-request/parse-notify. Modeled directly on
 * Espressif's Bluedroid `gatt_client` example
 * (examples/bluetooth/bluedroid/ble/gatt_client/main/gattc_demo.c).
 *
 * TODO(bench, unverified without real hardware):
 *   - Peer BLE address type: JBD_BMS_ADDR_TYPE below is a guess
 *     (BLE_ADDR_TYPE_PUBLIC). If connections never establish, try
 *     BLE_ADDR_TYPE_RANDOM instead -- easy to see with a phone BLE
 *     scanner app against the actual module.
 *   - Service/characteristic UUIDs (0xFF00/0xFF01/0xFF02) are the values
 *     commonly used by JBD/Xiaoxiang BLE UART modules across the open
 *     ecosystem (ESPHome's jbd_bms component and others), not yet
 *     confirmed against these specific batteries.
 *   - Whether the write characteristic wants write-with-response or
 *     write-without-response; this uses write-without-response
 *     (ESP_GATT_WRITE_TYPE_NO_RSP), the more common case for this class
 *     of module.
 */
#include "jbd_bms_client.h"

#include <string.h>

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_defs.h"
#include "esp_gattc_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "jbd_bms_client";

#define JBD_BMS_ADDR_TYPE BLE_ADDR_TYPE_PUBLIC

#define JBD_SVC_UUID        0xFF00
#define JBD_CHR_NOTIFY_UUID 0xFF01
#define JBD_CHR_WRITE_UUID  0xFF02

#define JBD_TASK_STACK        4096
#define JBD_TASK_PRIO         9
#define JBD_TASK_PERIOD_MS    1000
#define JBD_RECONNECT_MS      5000
#define JBD_HEALTHY_WINDOW_MS 15000
#define JBD_REASSEMBLE_BUF_LEN 136
#define JBD_ATTR_RESULT_MAX    2   /* JBD peripherals expose exactly one of each -- small margin */

typedef enum {
    SLOT_DISABLED = 0,   /* placeholder MAC -- never touched */
    SLOT_IDLE,            /* app registered, waiting to (re)connect */
    SLOT_CONNECTING,       /* esp_ble_gattc_open() called, discovering service/chars */
    SLOT_READY,            /* subscribed, polling on schedule */
} slot_state_t;

typedef struct {
    bool             enabled;
    esp_bd_addr_t    bda;
    esp_gatt_if_t    gattc_if;
    slot_state_t     state;
    uint16_t         conn_id;
    uint16_t         svc_start_handle;
    uint16_t         svc_end_handle;
    uint16_t         notify_char_handle;
    uint16_t         write_char_handle;

    uint8_t  reassemble_buf[JBD_REASSEMBLE_BUF_LEN];
    size_t   reassemble_len;

    TickType_t next_action_tick;   /* reconnect backoff / next poll time */

    jbd_bms_status_t status;
    bool             status_valid;
    TickType_t       last_valid_tick;
} jbd_slot_t;

static jbd_slot_t s_slots[JBD_BMS_MAX_BATTERIES];
static SemaphoreHandle_t s_status_mutex;
static uint32_t s_poll_interval_ms = CONFIG_FIREFLY_BATTERY_POLL_INTERVAL_MS;

static const esp_bt_uuid_t k_svc_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid = { .uuid16 = JBD_SVC_UUID },
};
static const esp_bt_uuid_t k_notify_char_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid = { .uuid16 = JBD_CHR_NOTIFY_UUID },
};
static const esp_bt_uuid_t k_write_char_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid = { .uuid16 = JBD_CHR_WRITE_UUID },
};
static const esp_bt_uuid_t k_cccd_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid = { .uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG },
};

static bool parse_mac(const char *str, uint8_t mac[6])
{
    unsigned b[6];
    if (sscanf(str, "%x:%x:%x:%x:%x:%x",
              &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        mac[i] = (uint8_t)b[i];
    }
    return true;
}

static void reset_slot_for_retry(jbd_slot_t *slot)
{
    slot->state = SLOT_IDLE;
    slot->reassemble_len = 0;
    slot->next_action_tick = xTaskGetTickCount() + pdMS_TO_TICKS(JBD_RECONNECT_MS);
}

/* ---------------------------------------------------------- notify data - */

static void on_notify_data(jbd_slot_t *slot, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        const uint8_t b = data[i];
        if (slot->reassemble_len == 0 && b != JBD_BMS_START_BYTE) {
            continue;   /* not the start of a frame yet -- drop stray bytes */
        }
        if (slot->reassemble_len < JBD_REASSEMBLE_BUF_LEN) {
            slot->reassemble_buf[slot->reassemble_len++] = b;
        } else {
            slot->reassemble_len = 0;   /* overflowed -- resync on next 0xDD */
            continue;
        }
        if (b == JBD_BMS_END_BYTE) {
            jbd_bms_status_t parsed;
            if (jbd_bms_parse_basic_info(slot->reassemble_buf, slot->reassemble_len, &parsed)) {
                xSemaphoreTake(s_status_mutex, portMAX_DELAY);
                slot->status = parsed;
                slot->status_valid = true;
                slot->last_valid_tick = xTaskGetTickCount();
                xSemaphoreGive(s_status_mutex);
            }
            slot->reassemble_len = 0;
        }
    }
}

/* ----------------------------------------------------------- GATTC event- */

static void on_gattc_event(jbd_slot_t *slot, esp_gattc_cb_event_t event,
                           esp_ble_gattc_cb_param_t *p)
{
    switch (event) {
    case ESP_GATTC_CONNECT_EVT:
        slot->conn_id = p->connect.conn_id;
        memcpy(slot->bda, p->connect.remote_bda, sizeof(esp_bd_addr_t));
        slot->svc_start_handle = 0;
        slot->svc_end_handle = 0;
        slot->notify_char_handle = 0;
        slot->write_char_handle = 0;
        slot->reassemble_len = 0;
        slot->state = SLOT_CONNECTING;
        esp_ble_gattc_send_mtu_req(slot->gattc_if, slot->conn_id);
        break;

    case ESP_GATTC_DIS_SRVC_CMPL_EVT:
        if (p->dis_srvc_cmpl.status != ESP_GATT_OK) {
            ESP_LOGW(TAG, "service discovery failed, status %d", p->dis_srvc_cmpl.status);
            break;
        }
        esp_ble_gattc_search_service(slot->gattc_if, slot->conn_id, (esp_bt_uuid_t *)&k_svc_uuid);
        break;

    case ESP_GATTC_SEARCH_RES_EVT:
        if (p->search_res.srvc_id.uuid.len == ESP_UUID_LEN_16 &&
            p->search_res.srvc_id.uuid.uuid.uuid16 == JBD_SVC_UUID) {
            slot->svc_start_handle = p->search_res.start_handle;
            slot->svc_end_handle = p->search_res.end_handle;
        }
        break;

    case ESP_GATTC_SEARCH_CMPL_EVT: {
        if (p->search_cmpl.status != ESP_GATT_OK || slot->svc_start_handle == 0) {
            ESP_LOGW(TAG, "JBD service (0xFF00) not found -- terminating connection");
            esp_ble_gattc_close(slot->gattc_if, slot->conn_id);
            break;
        }

        esp_gattc_char_elem_t chars[JBD_ATTR_RESULT_MAX];
        uint16_t count = JBD_ATTR_RESULT_MAX;
        if (esp_ble_gattc_get_char_by_uuid(slot->gattc_if, slot->conn_id,
                                           slot->svc_start_handle, slot->svc_end_handle,
                                           k_notify_char_uuid, chars, &count) == ESP_GATT_OK &&
            count > 0) {
            slot->notify_char_handle = chars[0].char_handle;
        }

        count = JBD_ATTR_RESULT_MAX;
        if (esp_ble_gattc_get_char_by_uuid(slot->gattc_if, slot->conn_id,
                                           slot->svc_start_handle, slot->svc_end_handle,
                                           k_write_char_uuid, chars, &count) == ESP_GATT_OK &&
            count > 0) {
            slot->write_char_handle = chars[0].char_handle;
        }

        if (slot->notify_char_handle == 0 || slot->write_char_handle == 0) {
            ESP_LOGW(TAG, "JBD notify/write characteristic not found -- terminating connection");
            esp_ble_gattc_close(slot->gattc_if, slot->conn_id);
            break;
        }
        esp_ble_gattc_register_for_notify(slot->gattc_if, slot->bda, slot->notify_char_handle);
        break;
    }

    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
        if (p->reg_for_notify.status != ESP_GATT_OK) {
            ESP_LOGW(TAG, "notify registration failed, status %d", p->reg_for_notify.status);
            break;
        }
        esp_gattc_descr_elem_t descrs[JBD_ATTR_RESULT_MAX];
        uint16_t count = JBD_ATTR_RESULT_MAX;
        if (esp_ble_gattc_get_descr_by_char_handle(slot->gattc_if, slot->conn_id,
                                                    p->reg_for_notify.handle, k_cccd_uuid,
                                                    descrs, &count) != ESP_GATT_OK ||
            count == 0) {
            ESP_LOGW(TAG, "no CCCD found on notify characteristic -- giving up on this connection");
            esp_ble_gattc_close(slot->gattc_if, slot->conn_id);
            break;
        }
        uint8_t enable[2] = { 0x01, 0x00 };
        esp_ble_gattc_write_char_descr(slot->gattc_if, slot->conn_id, descrs[0].handle,
                                       sizeof(enable), enable,
                                       ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
        slot->state = SLOT_READY;
        slot->next_action_tick = xTaskGetTickCount();   /* poll ASAP */
        ESP_LOGI(TAG, "slot ready, subscribed to notify");
        break;
    }

    case ESP_GATTC_NOTIFY_EVT:
        on_notify_data(slot, p->notify.value, p->notify.value_len);
        break;

    case ESP_GATTC_DISCONNECT_EVT:
        ESP_LOGW(TAG, "disconnected, reason 0x%02x", p->disconnect.reason);
        reset_slot_for_retry(slot);
        break;

    default:
        break;
    }
}

static void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                esp_ble_gattc_cb_param_t *param)
{
    if (event == ESP_GATTC_REG_EVT) {
        const uint16_t app_id = param->reg.app_id;
        if (app_id >= JBD_BMS_MAX_BATTERIES) {
            return;
        }
        jbd_slot_t *slot = &s_slots[app_id];
        if (param->reg.status != ESP_GATT_OK) {
            ESP_LOGW(TAG, "battery %u: app register failed, status %d", app_id + 1,
                     param->reg.status);
            return;
        }
        slot->gattc_if = gattc_if;
        slot->state = SLOT_IDLE;
        slot->next_action_tick = xTaskGetTickCount();
        return;
    }

    for (uint8_t i = 0; i < JBD_BMS_MAX_BATTERIES; i++) {
        if (s_slots[i].enabled && s_slots[i].gattc_if == gattc_if) {
            on_gattc_event(&s_slots[i], event, param);
            return;
        }
    }
}

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    /* No scanning/advertising here -- batteries are connected directly by
     * known address, so nothing in this project currently needs a GAP
     * event, but Bluedroid requires a registered callback regardless. */
    (void)event;
    (void)param;
}

static void try_connect(jbd_slot_t *slot)
{
    /* esp_ble_gattc_open() requires BT_BLE_42_FEATURES_SUPPORTED, which is
     * off by default on ESP32-S3 (BT_BLE_50_FEATURES_SUPPORTED is the
     * default instead) -- use the BLE 5.0 "enhanced" connect API, same
     * call shape as the official Bluedroid gattc_demo example's direct
     * (non-scan) connect path. */
    esp_ble_gatt_creat_conn_params_t conn_params = { 0 };
    memcpy(conn_params.remote_bda, slot->bda, sizeof(esp_bd_addr_t));
    conn_params.remote_addr_type = JBD_BMS_ADDR_TYPE;
    conn_params.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
    conn_params.is_direct = true;
    conn_params.is_aux = false;
    conn_params.phy_mask = 0x0;

    esp_err_t err = esp_ble_gattc_enh_open(slot->gattc_if, &conn_params);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_ble_gattc_enh_open failed, err=%s", esp_err_to_name(err));
        reset_slot_for_retry(slot);
        return;
    }
    slot->state = SLOT_CONNECTING;
    slot->next_action_tick = xTaskGetTickCount() + pdMS_TO_TICKS(JBD_RECONNECT_MS);
}

static void try_poll(jbd_slot_t *slot)
{
    uint8_t req[JBD_BMS_REQUEST_LEN];
    jbd_bms_build_request(JBD_BMS_REG_BASIC_INFO, req, sizeof(req));
    slot->reassemble_len = 0;
    esp_err_t err = esp_ble_gattc_write_char(slot->gattc_if, slot->conn_id, slot->write_char_handle,
                                             sizeof(req), req,
                                             ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "poll write failed, err=%s", esp_err_to_name(err));
    }
    slot->next_action_tick = xTaskGetTickCount() + pdMS_TO_TICKS(s_poll_interval_ms);
}

/* ------------------------------------------------------------- own task -- */

static void jbd_bms_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(JBD_TASK_PERIOD_MS));
        const TickType_t now = xTaskGetTickCount();
        for (uint8_t i = 0; i < JBD_BMS_MAX_BATTERIES; i++) {
            jbd_slot_t *slot = &s_slots[i];
            if (!slot->enabled) {
                continue;
            }
            if (slot->state == SLOT_IDLE && (int32_t)(now - slot->next_action_tick) >= 0) {
                try_connect(slot);
            } else if (slot->state == SLOT_READY && (int32_t)(now - slot->next_action_tick) >= 0) {
                try_poll(slot);
            }
        }
    }
}

/* ------------------------------------------------------------- lifecycle - */

esp_err_t jbd_bms_client_start(void)
{
    s_status_mutex = xSemaphoreCreateMutex();
    if (s_status_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const char *macs[JBD_BMS_MAX_BATTERIES] = {
        CONFIG_FIREFLY_BATTERY_1_MAC,
        CONFIG_FIREFLY_BATTERY_2_MAC,
        CONFIG_FIREFLY_BATTERY_3_MAC,
    };
    uint8_t enabled_count = 0;
    for (uint8_t i = 0; i < JBD_BMS_MAX_BATTERIES; i++) {
        uint8_t mac[6];
        if (!parse_mac(macs[i], mac)) {
            ESP_LOGW(TAG, "battery %u: bad MAC '%s', skipping", i + 1, macs[i]);
            continue;
        }
        static const uint8_t placeholder[6] = { 0, 0, 0, 0, 0, 0 };
        if (memcmp(mac, placeholder, sizeof(mac)) == 0) {
            ESP_LOGI(TAG, "battery %u: MAC unconfigured, skipping", i + 1);
            continue;
        }
        memcpy(s_slots[i].bda, mac, sizeof(esp_bd_addr_t));
        s_slots[i].enabled = true;
        s_slots[i].gattc_if = ESP_GATT_IF_NONE;
        enabled_count++;
    }
    if (enabled_count == 0) {
        ESP_LOGI(TAG, "no batteries configured -- BLE client idle until MACs are set");
    }

    esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "esp_bt_controller_mem_release: %s", esp_err_to_name(err));
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_controller_init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_controller_enable failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_bluedroid_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_bluedroid_init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_bluedroid_enable();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_bluedroid_enable failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));
    ESP_ERROR_CHECK(esp_ble_gattc_register_callback(gattc_event_handler));

    for (uint8_t i = 0; i < JBD_BMS_MAX_BATTERIES; i++) {
        if (s_slots[i].enabled) {
            esp_err_t reg_err = esp_ble_gattc_app_register(i);
            if (reg_err != ESP_OK) {
                ESP_LOGW(TAG, "battery %u: app register call failed: %s", i + 1,
                         esp_err_to_name(reg_err));
            }
        }
    }

    const BaseType_t ok = xTaskCreate(jbd_bms_task, "jbd_bms", JBD_TASK_STACK, NULL,
                                      JBD_TASK_PRIO, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "JBD-BMS client started (%u batteries configured)", enabled_count);
    return ESP_OK;
}

bool jbd_bms_get_status(uint8_t index, jbd_bms_status_t *out)
{
    if (index >= JBD_BMS_MAX_BATTERIES || out == NULL || s_status_mutex == NULL) {
        return false;
    }
    jbd_slot_t *slot = &s_slots[index];
    if (!slot->enabled) {
        return false;
    }

    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    const bool valid = slot->status_valid;
    if (valid) {
        *out = slot->status;
    }
    xSemaphoreGive(s_status_mutex);
    return valid;
}

bool jbd_bms_healthy(uint8_t index)
{
    if (index >= JBD_BMS_MAX_BATTERIES) {
        return false;
    }
    jbd_slot_t *slot = &s_slots[index];
    if (!slot->enabled || !slot->status_valid) {
        return false;
    }
    return (xTaskGetTickCount() - slot->last_valid_tick) < pdMS_TO_TICKS(JBD_HEALTHY_WINDOW_MS);
}
