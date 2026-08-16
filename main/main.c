/*
 * firefly-touch — RV-C touchscreen wall panel for a Firefly G6A multiplex
 * coach. Each CAN-connected panel is an independent peer node on the
 * 250 kbps RV-C bus; there is no hub, the bus is the shared state.
 *
 * A PANEL_HAS_CAN=0 panel (panel_config.h) has no CAN wiring at all — it
 * relays commands/status to/from a PANEL_IS_BRIDGE panel over ESP-NOW
 * instead (components/espnow_link). mid_coach is PANEL_IS_BRIDGE=1: it
 * runs the normal CAN stack AND the ESP-NOW side, forwarding one to the
 * other.
 *
 * Task map:
 *   core 0: twai_rx_task (12) / twai_tx_task (11) / espnow_rx_task (10,
 *           if PANEL_IS_BRIDGE or !PANEL_HAS_CAN) / state_mgr (9)
 *   core 1: LVGL task via esp_lvgl_port (4)
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "esp_log.h"

#include "board_4_3b.h"
#include "panel_config.h"
#include "ui.h"

#if PANEL_HAS_CAN
#include "twai_tasks.h"
#endif

#if PANEL_IS_BRIDGE
#include "state_manager.h"
#endif

#if !PANEL_HAS_CAN || PANEL_IS_BRIDGE
#include "espnow_link.h"
#endif

static const char *TAG = "main";

#if !PANEL_HAS_CAN
/* Remote panel: apply a status update relayed from the bridge directly to
 * the UI. There is no local state_mgr/bus-truth table here — the bridge
 * panel owns that; this is purely a display of what it reports.
 * ui_on_status() takes the LVGL lock itself, so this is safe to call from
 * espnow_rx_task's context. */
static void remote_status_rx(const espnow_status_msg_t *msg, void *ctx)
{
    (void)ctx;
    ui_on_status(msg->instance, msg->level, msg->on);
}
#endif

#if PANEL_IS_BRIDGE
#define ESPNOW_RESYNC_PERIOD_MS 30000

/* Remote panel -> bridge: relay onto the real bus exactly like a local
 * button press would (twai_enqueue_dimmer_cmd() is the same non-blocking,
 * drop-if-full entry point ui.c uses). */
static void bridge_cmd_rx(const espnow_cmd_msg_t *msg, void *ctx)
{
    (void)ctx;
    twai_enqueue_dimmer_cmd(msg->instance, msg->command, msg->level, msg->duration);
}

/* Bridge -> remote panel: forward every real state_mgr change. */
static void bridge_forward_status(uint8_t instance, uint8_t level, bool on, void *ctx)
{
    (void)ctx;
    const espnow_status_msg_t msg = { .instance = instance, .level = level, .on = on };
    espnow_link_send_status(&msg);
}

/* Status broadcasts are best-effort (no ack, same as RV-C status frames
 * themselves) — periodically push a full snapshot so a remote panel that
 * missed one, or just rebooted/reconnected, self-heals instead of staying
 * stale until the next incidental bus change. */
static void bridge_resync_timer_cb(TimerHandle_t t)
{
    (void)t;
    state_manager_for_each_known(bridge_forward_status, NULL);
}
#endif

void app_main(void)
{
    ESP_LOGI(TAG, "firefly-touch panel '%s' (index %d)", PANEL_NAME, PANEL_INDEX);

    /* Display + touch + LVGL (core 1). */
    ESP_ERROR_CHECK(board_display_init());

    /* Build the screen before bus/link traffic starts flowing so the first
     * status update lands on live widgets. */
    ui_init();

#if PANEL_HAS_CAN
    ESP_LOGI(TAG, "RV-C source addr 0x%02X", PANEL_SOURCE_ADDR);
    ESP_ERROR_CHECK(board_twai_init());
    ESP_ERROR_CHECK(twai_tasks_start());
#endif

#if PANEL_IS_BRIDGE
    ESP_ERROR_CHECK(espnow_link_init(true));
    espnow_link_set_cmd_rx_cb(bridge_cmd_rx, NULL);
    state_manager_register_status_sink(bridge_forward_status, NULL);
    TimerHandle_t resync_timer = xTimerCreate(
        "espnow_resync", pdMS_TO_TICKS(ESPNOW_RESYNC_PERIOD_MS), pdTRUE, NULL,
        bridge_resync_timer_cb);
    if (resync_timer != NULL) {
        xTimerStart(resync_timer, 0);
    } else {
        ESP_LOGW(TAG, "failed to create ESP-NOW resync timer");
    }
#elif !PANEL_HAS_CAN
    ESP_ERROR_CHECK(espnow_link_init(false));
    espnow_link_set_status_rx_cb(remote_status_rx, NULL);
#endif

    ESP_LOGI(TAG, "up");
}
