#include "espnow_link.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "espnow_link";

#define ESPNOW_TASK_CORE        0
#define ESPNOW_TASK_PRIO        10
#define ESPNOW_TASK_STACK       4096
#define RX_QUEUE_LEN            16
#define LINK_HEALTHY_WINDOW_MS  5000

typedef enum {
    ESPNOW_FRAME_CMD = 1,
    ESPNOW_FRAME_STATUS = 2,
} espnow_frame_type_t;

typedef struct {
    uint8_t type;
    union {
        espnow_cmd_msg_t    cmd;
        espnow_status_msg_t status;
    };
} espnow_frame_t;

static QueueHandle_t s_rx_queue;
static uint8_t s_peer_mac[6];

static espnow_cmd_rx_cb_t s_cmd_cb;
static void *s_cmd_cb_ctx;
static espnow_status_rx_cb_t s_status_cb;
static void *s_status_cb_ctx;

/* 32-bit aligned tick write/read is atomic on Xtensa; no lock needed
 * (same pattern as state_manager_bus_healthy()). */
static volatile TickType_t s_last_rx_tick;
static volatile bool s_rx_seen;

/* Copies at most `len` bytes of a Kconfig string into a fixed-size key
 * buffer, zero-padding if shorter. Not strncpy(): the 16-byte PMK/LMK keys
 * are raw bytes, not C strings, and a 16-char default exactly fills the
 * buffer with no room for a NUL — strncpy() warns (-Werror=stringop-
 * truncation) about exactly that, correctly, since it's not what strncpy
 * is for. */
static void copy_key(uint8_t *dst, size_t len, const char *src)
{
    memset(dst, 0, len);
    memcpy(dst, src, strnlen(src, len));
}

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

/* Runs in the WiFi driver's internal task context — never blocks, just
 * hands the frame to espnow_rx_task like twai_rx_task does for the bus. */
static void espnow_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    (void)info;
    if ((size_t)len != sizeof(espnow_frame_t)) {
        ESP_LOGW(TAG, "dropped frame: bad length %d (expected %u)",
                 len, (unsigned)sizeof(espnow_frame_t));
        return;
    }
    espnow_frame_t frame;
    memcpy(&frame, data, sizeof(frame));
    if (xQueueSend(s_rx_queue, &frame, 0) != pdTRUE) {
        ESP_LOGW(TAG, "rx queue full, dropped frame type %u", frame.type);
    }
}

static void espnow_rx_task(void *arg)
{
    (void)arg;
    espnow_frame_t frame;

    for (;;) {
        if (xQueueReceive(s_rx_queue, &frame, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        s_last_rx_tick = xTaskGetTickCount();
        s_rx_seen = true;

        if (frame.type == ESPNOW_FRAME_CMD && s_cmd_cb != NULL) {
            s_cmd_cb(&frame.cmd, s_cmd_cb_ctx);
        } else if (frame.type == ESPNOW_FRAME_STATUS && s_status_cb != NULL) {
            s_status_cb(&frame.status, s_status_cb_ctx);
        }
    }
}

static bool espnow_send_frame(const espnow_frame_t *frame)
{
    const esp_err_t err = esp_now_send(s_peer_mac, (const uint8_t *)frame, sizeof(*frame));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_now_send failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool espnow_link_send_cmd(const espnow_cmd_msg_t *msg)
{
    espnow_frame_t frame = { .type = ESPNOW_FRAME_CMD };
    frame.cmd = *msg;
    return espnow_send_frame(&frame);
}

bool espnow_link_send_status(const espnow_status_msg_t *msg)
{
    espnow_frame_t frame = { .type = ESPNOW_FRAME_STATUS };
    frame.status = *msg;
    return espnow_send_frame(&frame);
}

bool espnow_link_healthy(void)
{
    if (!s_rx_seen) {
        return false;
    }
    return (xTaskGetTickCount() - s_last_rx_tick) < pdMS_TO_TICKS(LINK_HEALTHY_WINDOW_MS);
}

void espnow_link_set_cmd_rx_cb(espnow_cmd_rx_cb_t cb, void *ctx)
{
    s_cmd_cb = cb;
    s_cmd_cb_ctx = ctx;
}

void espnow_link_set_status_rx_cb(espnow_status_rx_cb_t cb, void *ctx)
{
    s_status_cb = cb;
    s_status_cb_ctx = ctx;
}

esp_err_t espnow_link_init(bool is_bridge)
{
    if (!parse_mac(CONFIG_FIREFLY_ESPNOW_PEER_MAC, s_peer_mac)) {
        ESP_LOGE(TAG, "bad CONFIG_FIREFLY_ESPNOW_PEER_MAC '%s' (expected AA:BB:CC:DD:EE:FF)",
                 CONFIG_FIREFLY_ESPNOW_PEER_MAC);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    const wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(CONFIG_FIREFLY_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));

    s_rx_queue = xQueueCreate(RX_QUEUE_LEN, sizeof(espnow_frame_t));
    if (s_rx_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_now_init());

    /* PMK/LMK are zero-padded/truncated to the required 16 raw bytes.
     * CHANGE THE DEFAULTS before deploying — these frames actuate real
     * loads. */
    uint8_t pmk[16];
    copy_key(pmk, sizeof(pmk), CONFIG_FIREFLY_ESPNOW_PMK);
    ESP_ERROR_CHECK(esp_now_set_pmk(pmk));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, s_peer_mac, sizeof(s_peer_mac));
    peer.channel = CONFIG_FIREFLY_ESPNOW_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = true;
    copy_key(peer.lmk, sizeof(peer.lmk), CONFIG_FIREFLY_ESPNOW_LMK);
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));

    const BaseType_t ok = xTaskCreatePinnedToCore(
        espnow_rx_task, "espnow_rx", ESPNOW_TASK_STACK, NULL,
        ESPNOW_TASK_PRIO, NULL, ESPNOW_TASK_CORE);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "ESP-NOW link up (%s), peer %02X:%02X:%02X:%02X:%02X:%02X, channel %d",
             is_bridge ? "bridge" : "remote",
             s_peer_mac[0], s_peer_mac[1], s_peer_mac[2],
             s_peer_mac[3], s_peer_mac[4], s_peer_mac[5],
             CONFIG_FIREFLY_ESPNOW_CHANNEL);
    return ESP_OK;
}
