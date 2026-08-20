/*
 * jbd_bms_client — NimBLE central role, up to JBD_BMS_MAX_BATTERIES fixed
 * peripherals. Each slot runs an independent connect -> discover service
 * (0xFF00) -> discover characteristics (notify 0xFF01, write 0xFF02) ->
 * discover the notify characteristic's CCCD -> enable notifications ->
 * periodic write-request/parse-notify state machine. Modeled on the
 * connect/discover chain in Espressif's NimBLE "blecent" example.
 *
 * TODO(bench, unverified without real hardware):
 *   - Peer BLE address type: JBD_BMS_ADDR_TYPE below is a guess
 *     (BLE_ADDR_PUBLIC). If connections never establish, try
 *     BLE_ADDR_RANDOM instead -- easy to see with a phone BLE scanner app
 *     against the actual module.
 *   - Service/characteristic UUIDs (0xFF00/0xFF01/0xFF02) are the values
 *     commonly used by JBD/Xiaoxiang BLE UART modules across the open
 *     ecosystem (ESPHome's jbd_bms component and others), not yet
 *     confirmed against these specific batteries.
 *   - Whether the write characteristic wants write-with-response or
 *     write-without-response; this uses write-without-response
 *     (ble_gattc_write_no_rsp_flat), the more common case for this class
 *     of module.
 */
#include "jbd_bms_client.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "services/gap/ble_svc_gap.h"
#include "sdkconfig.h"

static const char *TAG = "jbd_bms_client";

#define JBD_BMS_ADDR_TYPE BLE_ADDR_PUBLIC

#define JBD_SVC_UUID       0xFF00
#define JBD_CHR_NOTIFY_UUID 0xFF01
#define JBD_CHR_WRITE_UUID  0xFF02
#define JBD_DSC_CCCD_UUID   0x2902

#define JBD_TASK_STACK       4096
#define JBD_TASK_PRIO        9
#define JBD_TASK_PERIOD_MS   1000
#define JBD_RECONNECT_MS     5000
#define JBD_HEALTHY_WINDOW_MS 15000
#define JBD_REASSEMBLE_BUF_LEN 136

typedef enum {
    SLOT_DISABLED = 0,   /* placeholder MAC -- never touched */
    SLOT_IDLE,            /* waiting to (re)connect */
    SLOT_CONNECTING,
    SLOT_DISCOVERING,
    SLOT_READY,           /* subscribed, polling on schedule */
} slot_state_t;

typedef struct {
    bool          enabled;
    ble_addr_t    addr;
    slot_state_t  state;
    uint16_t      conn_handle;
    uint16_t      svc_start_handle;
    uint16_t      svc_end_handle;
    uint16_t      notify_val_handle;
    uint16_t      notify_end_handle;   /* for CCCD discovery range */
    uint16_t      write_val_handle;
    uint16_t      cccd_handle;

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

/* NimBLE addresses are little-endian on the wire; a MAC string is written
 * big-endian (AA:BB:...), so reverse byte order when filling ble_addr_t. */
static void mac_to_ble_addr(const uint8_t mac[6], ble_addr_t *out)
{
    out->type = JBD_BMS_ADDR_TYPE;
    for (int i = 0; i < 6; i++) {
        out->val[i] = mac[5 - i];
    }
}

static void reset_slot_for_retry(jbd_slot_t *slot)
{
    slot->state = SLOT_IDLE;
    slot->conn_handle = BLE_HS_CONN_HANDLE_NONE;
    slot->reassemble_len = 0;
    slot->next_action_tick = xTaskGetTickCount() + pdMS_TO_TICKS(JBD_RECONNECT_MS);
}

/* ---------------------------------------------------------- GATT chain --- */

static int on_dsc_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                       uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    jbd_slot_t *slot = arg;
    (void)conn_handle;
    (void)chr_val_handle;

    if (error->status == 0 && dsc != NULL &&
        ble_uuid_u16(&dsc->uuid.u) == JBD_DSC_CCCD_UUID) {
        slot->cccd_handle = dsc->handle;
    } else if (error->status != 0) {
        /* BLE_HS_EDONE (or any terminal status) -- discovery finished. */
        if (slot->cccd_handle != 0) {
            uint8_t enable[2] = { 0x01, 0x00 };
            ble_gattc_write_flat(slot->conn_handle, slot->cccd_handle,
                                 enable, sizeof(enable), NULL, NULL);
            slot->state = SLOT_READY;
            slot->next_action_tick = xTaskGetTickCount();   /* poll ASAP */
            ESP_LOGI(TAG, "slot ready, subscribed to notify (handle %u)", slot->cccd_handle);
        } else {
            ESP_LOGW(TAG, "no CCCD found on notify characteristic -- giving up on this connection");
            ble_gap_terminate(slot->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
    return 0;
}

static int on_chr_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr, void *arg)
{
    jbd_slot_t *slot = arg;
    (void)conn_handle;

    if (error->status == 0 && chr != NULL) {
        const uint16_t uuid16 = ble_uuid_u16(&chr->uuid.u);
        if (uuid16 == JBD_CHR_NOTIFY_UUID) {
            slot->notify_val_handle = chr->val_handle;
        } else if (uuid16 == JBD_CHR_WRITE_UUID) {
            slot->write_val_handle = chr->val_handle;
        }
        return 0;
    }

    /* Characteristic discovery finished (error->status == BLE_HS_EDONE on
     * success, or a real error). */
    if (slot->notify_val_handle == 0) {
        ESP_LOGW(TAG, "JBD notify characteristic (0xFF01) not found -- terminating");
        ble_gap_terminate(slot->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }
    ble_gattc_disc_all_dscs(slot->conn_handle, slot->notify_val_handle,
                            slot->notify_end_handle, on_dsc_disc, slot);
    return 0;
}

static int on_svc_disc(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_svc *svc, void *arg)
{
    jbd_slot_t *slot = arg;
    (void)conn_handle;

    if (error->status != 0 || svc == NULL) {
        if (slot->svc_start_handle == 0) {
            ESP_LOGW(TAG, "JBD service (0xFF00) not found -- giving up on this connection");
            ble_gap_terminate(slot->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return 0;
    }

    slot->svc_start_handle = svc->start_handle;
    slot->svc_end_handle = svc->end_handle;
    slot->notify_end_handle = svc->end_handle;
    ble_gattc_disc_all_chrs(slot->conn_handle, svc->start_handle, svc->end_handle,
                            on_chr_disc, slot);
    return 0;
}

/* --------------------------------------------------------- notify data --- */

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

/* ------------------------------------------------------------ GAP event -- */

static int on_gap_event(struct ble_gap_event *event, void *arg)
{
    jbd_slot_t *slot = arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            slot->conn_handle = event->connect.conn_handle;
            slot->state = SLOT_DISCOVERING;
            slot->svc_start_handle = 0;
            slot->svc_end_handle = 0;
            slot->notify_val_handle = 0;
            slot->write_val_handle = 0;
            slot->cccd_handle = 0;
            ble_gattc_disc_svc_by_uuid(slot->conn_handle,
                                       BLE_UUID16_DECLARE(JBD_SVC_UUID),
                                       on_svc_disc, slot);
        } else {
            ESP_LOGW(TAG, "connect failed, status=%d", event->connect.status);
            reset_slot_for_retry(slot);
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "disconnected, reason=%d", event->disconnect.reason);
        reset_slot_for_retry(slot);
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX:
        if (event->notify_rx.attr_handle == slot->notify_val_handle) {
            const struct os_mbuf *om = event->notify_rx.om;
            uint8_t chunk[64];
            uint16_t chunk_len = OS_MBUF_PKTLEN(om) > sizeof(chunk)
                                     ? sizeof(chunk) : (uint16_t)OS_MBUF_PKTLEN(om);
            os_mbuf_copydata((struct os_mbuf *)om, 0, chunk_len, chunk);
            on_notify_data(slot, chunk, chunk_len);
        }
        return 0;

    default:
        return 0;
    }
}

static void try_connect(jbd_slot_t *slot)
{
    slot->state = SLOT_CONNECTING;
    int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &slot->addr, 5000, NULL,
                             on_gap_event, slot);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_connect failed, rc=%d", rc);
        reset_slot_for_retry(slot);
    }
}

static void try_poll(jbd_slot_t *slot)
{
    uint8_t req[JBD_BMS_REQUEST_LEN];
    jbd_bms_build_request(JBD_BMS_REG_BASIC_INFO, req, sizeof(req));
    slot->reassemble_len = 0;
    int rc = ble_gattc_write_no_rsp_flat(slot->conn_handle, slot->write_val_handle,
                                         req, sizeof(req));
    if (rc != 0) {
        ESP_LOGW(TAG, "poll write failed, rc=%d", rc);
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

static void ble_app_on_sync(void)
{
    ble_hs_id_infer_auto(0, NULL);
    for (uint8_t i = 0; i < JBD_BMS_MAX_BATTERIES; i++) {
        if (s_slots[i].enabled) {
            s_slots[i].state = SLOT_IDLE;
            s_slots[i].next_action_tick = xTaskGetTickCount();
        }
    }
}

static void ble_app_on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset, reason=%d", reason);
}

static void nimble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

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
        mac_to_ble_addr(mac, &s_slots[i].addr);
        s_slots[i].enabled = true;
        s_slots[i].conn_handle = BLE_HS_CONN_HANDLE_NONE;
        enabled_count++;
    }
    if (enabled_count == 0) {
        ESP_LOGI(TAG, "no batteries configured -- BLE client idle until MACs are set");
    }

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.reset_cb = ble_app_on_reset;
    ble_hs_cfg.sync_cb = ble_app_on_sync;
    ble_svc_gap_init();
    ble_svc_gap_device_name_set("firefly-touch");

    nimble_port_freertos_init(nimble_host_task);

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
