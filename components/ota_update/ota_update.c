#include "ota_update.h"
#include "ota_manifest.h"

#include <limits.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "sdkconfig.h"

static const char *TAG = "ota_update";

#define OTA_TASK_CORE  0
#define OTA_TASK_PRIO  5
#define OTA_TASK_STACK 6144

#define OTA_NOTIFY_CHECK_REQUEST (1u << 0)
#define OTA_NOTIFY_WIFI_GOT_IP   (1u << 1)
#define OTA_NOTIFY_WIFI_FAIL     (1u << 2)

#define OTA_MANIFEST_BUF_LEN 512

static TaskHandle_t s_task;
static volatile ota_status_t s_status = OTA_STATUS_IDLE;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (s_task == NULL) {
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xTaskNotify(s_task, OTA_NOTIFY_WIFI_FAIL, eSetBits);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xTaskNotify(s_task, OTA_NOTIFY_WIFI_GOT_IP, eSetBits);
    }
}

/*
 * Brings Wi-Fi up in STA mode if nothing has done so yet. On a panel that
 * also runs espnow_link_init(), Wi-Fi is already initialized and started
 * (fixed on CONFIG_FIREFLY_ESPNOW_CHANNEL, no AP association) by the time
 * this runs — detected via esp_wifi_get_mode() and left alone.
 */
static void ensure_wifi_ready(void)
{
    static bool handlers_registered = false;

    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_ERR_WIFI_NOT_INIT) {
        esp_err_t err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_ERROR_CHECK(nvs_flash_erase());
            err = nvs_flash_init();
        }
        ESP_ERROR_CHECK(err);

        ESP_ERROR_CHECK(esp_netif_init());
        esp_err_t ev_err = esp_event_loop_create_default();
        if (ev_err != ESP_OK && ev_err != ESP_ERR_INVALID_STATE) {
            ESP_ERROR_CHECK(ev_err);
        }
        esp_netif_create_default_wifi_sta();

        const wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
    }

    if (!handlers_registered) {
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));
        handlers_registered = true;
    }
}

/* Restores the fixed ESP-NOW channel after a check, whether or not this
 * panel actually runs ESP-NOW — harmless either way. */
static void wifi_restore_espnow_channel(void)
{
    esp_wifi_disconnect();
    esp_wifi_set_channel(CONFIG_FIREFLY_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
}

static bool wifi_connect_and_wait(void)
{
    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, CONFIG_FIREFLY_OTA_WIFI_SSID,
            sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, CONFIG_FIREFLY_OTA_WIFI_PASSWORD,
            sizeof(wifi_config.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect() failed: %s", esp_err_to_name(err));
        return false;
    }

    uint32_t bits = 0;
    xTaskNotifyWait(ULONG_MAX, ULONG_MAX, &bits,
                     pdMS_TO_TICKS(CONFIG_FIREFLY_OTA_CONNECT_TIMEOUT_MS));
    return (bits & OTA_NOTIFY_WIFI_GOT_IP) != 0;
}

static bool fetch_manifest(ota_manifest_t *out)
{
    char buf[OTA_MANIFEST_BUF_LEN];
    esp_http_client_config_t cfg = {
        .url = CONFIG_FIREFLY_OTA_MANIFEST_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = CONFIG_FIREFLY_OTA_CONNECT_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "manifest: http client init failed");
        return false;
    }

    bool ok = false;
    if (esp_http_client_open(client, 0) != ESP_OK) {
        ESP_LOGE(TAG, "manifest: connection failed");
    } else {
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status != 200) {
            ESP_LOGE(TAG, "manifest: HTTP %d", status);
        } else {
            int read_len = esp_http_client_read_response(client, buf, sizeof(buf) - 1);
            if (read_len < 0) {
                ESP_LOGE(TAG, "manifest: read failed");
            } else {
                buf[read_len] = '\0';
                ok = ota_manifest_parse(buf, (size_t)read_len, out);
                if (!ok) {
                    ESP_LOGE(TAG, "manifest: parse failed (bad JSON or oversized field)");
                }
            }
        }
        esp_http_client_close(client);
    }
    esp_http_client_cleanup(client);
    return ok;
}

static void run_check_and_update(void)
{
    s_status = OTA_STATUS_CHECKING;
    ESP_LOGI(TAG, "OTA check starting");

    ensure_wifi_ready();

    if (!wifi_connect_and_wait()) {
        ESP_LOGW(TAG, "OTA check: WiFi connect failed or timed out");
        wifi_restore_espnow_channel();
        s_status = OTA_STATUS_FAILED;
        return;
    }

    ota_manifest_t manifest;
    if (!fetch_manifest(&manifest)) {
        wifi_restore_espnow_channel();
        s_status = OTA_STATUS_FAILED;
        return;
    }

    const esp_app_desc_t *running_desc = esp_app_get_description();
    if (!ota_manifest_is_update_available(running_desc->version, manifest.version)) {
        ESP_LOGI(TAG, "OTA: running %s, manifest %s -- up to date",
                 running_desc->version, manifest.version);
        wifi_restore_espnow_channel();
        s_status = OTA_STATUS_UP_TO_DATE;
        return;
    }

    ESP_LOGI(TAG, "OTA: update available %s -> %s, downloading from %s",
             running_desc->version, manifest.version, manifest.url);
    s_status = OTA_STATUS_UPDATING;

    esp_http_client_config_t ota_http_cfg = {
        .url = manifest.url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = CONFIG_FIREFLY_OTA_CONNECT_TIMEOUT_MS,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &ota_http_cfg,
    };
    esp_err_t err = esp_https_ota(&ota_cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA succeeded, rebooting");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }

    ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
    wifi_restore_espnow_channel();
    s_status = OTA_STATUS_FAILED;
}

static void ota_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t bits = 0;
        xTaskNotifyWait(ULONG_MAX, ULONG_MAX, &bits, portMAX_DELAY);
        if (bits & OTA_NOTIFY_CHECK_REQUEST) {
            run_check_and_update();
        }
    }
}

void ota_update_init(void)
{
    if (s_task != NULL) {
        return;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        ota_task, "ota_update", OTA_TASK_STACK, NULL, OTA_TASK_PRIO,
        &s_task, OTA_TASK_CORE);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create ota_update task");
        s_task = NULL;
        return;
    }

#if CONFIG_FIREFLY_OTA_CHECK_ON_BOOT
    ota_update_request_check();
#endif
}

bool ota_update_request_check(void)
{
    if (s_task == NULL) {
        return false;
    }
    if (s_status == OTA_STATUS_CHECKING || s_status == OTA_STATUS_UPDATING) {
        return false;
    }
    xTaskNotify(s_task, OTA_NOTIFY_CHECK_REQUEST, eSetBits);
    return true;
}

ota_status_t ota_update_get_status(void)
{
    return s_status;
}
