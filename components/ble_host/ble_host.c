#include "ble_host.h"

#include <stdbool.h>

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_log.h"

static const char *TAG = "ble_host";

/* Two clients today (batteries + Power Watchdog); the cap exists to keep the
 * tables static, not because more would be a problem. */
#define BLE_HOST_MAX_OBSERVERS 4

static esp_gap_ble_cb_t   s_gap_observers[BLE_HOST_MAX_OBSERVERS];
static uint8_t            s_gap_count;
static esp_gattc_cb_t     s_gattc_observers[BLE_HOST_MAX_OBSERVERS];
static uint8_t            s_gattc_count;
static bool               s_started;

static void gap_fanout(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    for (uint8_t i = 0; i < s_gap_count; i++) {
        s_gap_observers[i](event, param);
    }
}

static void gattc_fanout(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                         esp_ble_gattc_cb_param_t *param)
{
    for (uint8_t i = 0; i < s_gattc_count; i++) {
        s_gattc_observers[i](event, gattc_if, param);
    }
}

esp_err_t ble_host_add_gap_observer(esp_gap_ble_cb_t cb)
{
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_gap_count >= BLE_HOST_MAX_OBSERVERS) {
        return ESP_ERR_NO_MEM;
    }
    s_gap_observers[s_gap_count++] = cb;
    ESP_LOGI(TAG, "GAP observer %u registered", (unsigned)s_gap_count);
    return ESP_OK;
}

esp_err_t ble_host_add_gattc_observer(esp_gattc_cb_t cb)
{
    if (cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_gattc_count >= BLE_HOST_MAX_OBSERVERS) {
        return ESP_ERR_NO_MEM;
    }
    s_gattc_observers[s_gattc_count++] = cb;
    ESP_LOGI(TAG, "GATTC observer %u registered", (unsigned)s_gattc_count);
    return ESP_OK;
}

esp_err_t ble_host_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    /* Classic BT memory is dead weight on a BLE-only node — and on the
     * classic ESP32 (unlike the S3) it is a real BR/EDR radio's worth of
     * RAM. ESP_ERR_INVALID_STATE just means it is already released. */
    esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "esp_bt_controller_mem_release: %s", esp_err_to_name(err));
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_controller_init: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_controller_enable: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_bluedroid_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_bluedroid_init: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_bluedroid_enable();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_bluedroid_enable: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_ble_gap_register_callback(gap_fanout);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ble_gap_register_callback: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_ble_gattc_register_callback(gattc_fanout);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ble_gattc_register_callback: %s", esp_err_to_name(err));
        return err;
    }

    s_started = true;
    /* Deliberately no observer counts here: a client that starts the host
     * registers before this point, and any client starting later registers
     * after it, so a count logged now would undercount and read as a bug
     * that isn't one. Observers announce themselves as they are added. */
    ESP_LOGI(TAG, "BLE host up");
    return ESP_OK;
}
