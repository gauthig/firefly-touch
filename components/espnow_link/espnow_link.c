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
    ESPNOW_FRAME_TELEMETRY = 3,
} espnow_frame_type_t;

/*
 * Control frame (command/status). ⚠️ ITS SIZE IS PART OF THE WIRE FORMAT --
 * the RX path validates length, so growing this struct makes new firmware
 * incompatible with any node still running the old build, silently (frames
 * are just dropped). Telemetry therefore lives in its own frame below
 * rather than being bolted onto this union. Don't merge them.
 */
typedef struct {
    uint8_t type;
    union {
        espnow_cmd_msg_t    cmd;
        espnow_status_msg_t status;
    };
} espnow_frame_t;

/*
 * The control frame's size is load-bearing across firmware versions: panels
 * are flashed one at a time, and a size change makes an updated panel's
 * commands invisible to a panel still running the old build (the RX path
 * validates length, so they are dropped silently -- lights simply stop
 * responding, with no error on either side).
 *
 * Pin it. If this assert fires, you changed the wire format: either revert,
 * or accept that every node must be reflashed together and update the
 * expected size deliberately.
 */
_Static_assert(sizeof(espnow_frame_t) == 16,
               "espnow control frame size changed -- panels on older firmware "
               "will silently drop commands until they are all reflashed");

typedef struct {
    uint8_t type;               /* ESPNOW_FRAME_TELEMETRY */
    espnow_telem_msg_t telem;
} espnow_telem_frame_t;

/*
 * The telemetry frame's size is load-bearing for the same reason the
 * control frame's is, and the trap is easier to walk into: every producer
 * is a different physical node (the basement proxy broadcasts shore power,
 * batteries and solar; mid_coach broadcasts tanks), and they are flashed one
 * at a time. Widening the union to fit a new measurement would make every OLDER
 * producer's broadcasts fail this receiver's length check -- so adding
 * batteries would silently kill tank display until mid_coach was reflashed
 * too, with nothing logged anywhere.
 *
 * New measurements therefore have to fit the existing envelope. If this
 * assert fires, shrink the new struct rather than bumping the number.
 */
_Static_assert(sizeof(espnow_telem_frame_t) == 20,
               "espnow telemetry frame size changed -- broadcasts from nodes "
               "still on older firmware will be dropped as malformed");

/* Big enough for the largest frame; the queue carries raw bytes + length so
 * adding another frame type later doesn't require touching the queue. */
#define RX_ITEM_MAX 48
typedef struct {
    uint8_t len;
    uint8_t data[RX_ITEM_MAX];
} rx_item_t;

static const uint8_t k_broadcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static QueueHandle_t s_rx_queue;
static uint8_t s_peer_mac[6];
static bool    s_have_unicast_peer;

static espnow_cmd_rx_cb_t s_cmd_cb;
static void *s_cmd_cb_ctx;
static espnow_status_rx_cb_t s_status_cb;
static void *s_status_cb_ctx;
static espnow_telem_rx_cb_t s_telem_cb;
static void *s_telem_cb_ctx;

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
/* Expected wire length for a frame type, or 0 if the type is unknown. */
static size_t expected_len(uint8_t type)
{
    switch (type) {
    case ESPNOW_FRAME_CMD:
    case ESPNOW_FRAME_STATUS:
        return sizeof(espnow_frame_t);
    case ESPNOW_FRAME_TELEMETRY:
        return sizeof(espnow_telem_frame_t);
    default:
        return 0;
    }
}

static void espnow_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    (void)info;
    if (len <= 0 || (size_t)len > RX_ITEM_MAX) {
        return;
    }
    /* Validate length against what THIS type should be, rather than against
     * one global frame size -- that is what lets control and telemetry
     * frames have different sizes and coexist. An unknown type is ignored
     * quietly: a node running newer firmware may legitimately be
     * broadcasting something this build doesn't know about yet. */
    const size_t want = expected_len(data[0]);
    if (want == 0 || (size_t)len != want) {
        return;
    }

    rx_item_t item;
    item.len = (uint8_t)len;
    memcpy(item.data, data, (size_t)len);
    if (xQueueSend(s_rx_queue, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "rx queue full, dropped frame type %u", data[0]);
    }
}

static void espnow_rx_task(void *arg)
{
    (void)arg;
    rx_item_t item;

    for (;;) {
        if (xQueueReceive(s_rx_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* Telemetry is a broadcast from an unrelated node, so it must not
         * count as evidence that our unicast peer is alive -- otherwise a
         * dead bridge would still look "healthy" to a remote panel. */
        const bool is_telem = item.data[0] == ESPNOW_FRAME_TELEMETRY;
        if (!is_telem) {
            s_last_rx_tick = xTaskGetTickCount();
            s_rx_seen = true;
        }

        if (is_telem) {
            if (s_telem_cb != NULL) {
                espnow_telem_frame_t frame;
                memcpy(&frame, item.data, sizeof(frame));
                s_telem_cb(&frame.telem, s_telem_cb_ctx);
            }
            continue;
        }

        espnow_frame_t frame;
        memcpy(&frame, item.data, sizeof(frame));
        if (frame.type == ESPNOW_FRAME_CMD && s_cmd_cb != NULL) {
            s_cmd_cb(&frame.cmd, s_cmd_cb_ctx);
        } else if (frame.type == ESPNOW_FRAME_STATUS && s_status_cb != NULL) {
            s_status_cb(&frame.status, s_status_cb_ctx);
        }
    }
}

static bool espnow_send_frame(const espnow_frame_t *frame)
{
    if (!s_have_unicast_peer) {
        return false;   /* telemetry-only node has no control peer */
    }
    const esp_err_t err = esp_now_send(s_peer_mac, (const uint8_t *)frame, sizeof(*frame));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_now_send failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool espnow_link_send_telemetry(const espnow_telem_msg_t *msg)
{
    espnow_telem_frame_t frame = { .type = ESPNOW_FRAME_TELEMETRY };
    frame.telem = *msg;
    const esp_err_t err = esp_now_send(k_broadcast_mac, (const uint8_t *)&frame,
                                       sizeof(frame));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "telemetry broadcast failed: %s", esp_err_to_name(err));
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

void espnow_link_set_telem_rx_cb(espnow_telem_rx_cb_t cb, void *ctx)
{
    s_telem_cb = cb;
    s_telem_cb_ctx = ctx;
}

static const char *role_str(espnow_role_t role)
{
    switch (role) {
    case ESPNOW_ROLE_BRIDGE:    return "bridge";
    case ESPNOW_ROLE_REMOTE:    return "remote";
    case ESPNOW_ROLE_TELEMETRY: return "telemetry";
    default:                    return "?";
    }
}

esp_err_t espnow_link_init(espnow_role_t role)
{
    /* A telemetry-only producer broadcasts and never unicasts, so it has no
     * peer to configure -- don't fail it for a placeholder MAC it will
     * never use. */
    const bool wants_unicast = (role != ESPNOW_ROLE_TELEMETRY);
    if (wants_unicast && !parse_mac(CONFIG_FIREFLY_ESPNOW_PEER_MAC, s_peer_mac)) {
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

    s_rx_queue = xQueueCreate(RX_QUEUE_LEN, sizeof(rx_item_t));
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

    if (wants_unicast) {
        esp_now_peer_info_t peer = {0};
        memcpy(peer.peer_addr, s_peer_mac, sizeof(s_peer_mac));
        peer.channel = CONFIG_FIREFLY_ESPNOW_CHANNEL;
        peer.ifidx = WIFI_IF_STA;
        peer.encrypt = true;
        copy_key(peer.lmk, sizeof(peer.lmk), CONFIG_FIREFLY_ESPNOW_LMK);
        ESP_ERROR_CHECK(esp_now_add_peer(&peer));
        s_have_unicast_peer = true;
    }

    /* Broadcast peer, needed to SEND telemetry. Receiving broadcasts needs
     * no peer entry, but every node registers it anyway so any of them can
     * become a producer later without another config change.
     * encrypt = false is mandatory, not a choice: ESP-NOW cannot encrypt
     * broadcast frames. */
    esp_now_peer_info_t bcast = {0};
    memcpy(bcast.peer_addr, k_broadcast_mac, sizeof(k_broadcast_mac));
    bcast.channel = CONFIG_FIREFLY_ESPNOW_CHANNEL;
    bcast.ifidx = WIFI_IF_STA;
    bcast.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&bcast));

    const BaseType_t ok = xTaskCreatePinnedToCore(
        espnow_rx_task, "espnow_rx", ESPNOW_TASK_STACK, NULL,
        ESPNOW_TASK_PRIO, NULL, ESPNOW_TASK_CORE);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    if (wants_unicast) {
        ESP_LOGI(TAG, "ESP-NOW link up (%s), peer %02X:%02X:%02X:%02X:%02X:%02X, channel %d",
                 role_str(role),
                 s_peer_mac[0], s_peer_mac[1], s_peer_mac[2],
                 s_peer_mac[3], s_peer_mac[4], s_peer_mac[5],
                 CONFIG_FIREFLY_ESPNOW_CHANNEL);
    } else {
        ESP_LOGI(TAG, "ESP-NOW up (%s), broadcast only, channel %d",
                 role_str(role), CONFIG_FIREFLY_ESPNOW_CHANNEL);
    }
    return ESP_OK;
}
