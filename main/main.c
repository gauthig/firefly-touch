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

#include "board.h"
#include "panel_config.h"
#include "ui.h"

#if PANEL_HAS_CAN
#include "twai_tasks.h"
#endif

#if PANEL_IS_BRIDGE
#include "state_manager.h"
#endif

#if !PANEL_HAS_CAN || PANEL_IS_BRIDGE || PANEL_WANTS_TELEMETRY
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

#if !PANEL_HAS_CAN || PANEL_WANTS_TELEMETRY
/*
 * Read-only telemetry broadcast by some other node — the basement BLE proxy
 * (shore power) or the bridge panel (tank levels). Unlike status frames
 * these are unencrypted broadcasts from a node that is not our peer, which
 * is acceptable precisely because nothing here actuates anything: it is all
 * display.
 *
 * Tank telemetry deliberately re-enters through ui_on_tank_status(), the
 * same entry point the bridge's own CAN-fed tanks use, so a remote panel
 * with tank gauges needs no special handling at all.
 *
 * Also used by a CAN-connected panel that sets PANEL_WANTS_TELEMETRY: the
 * battery packs and the Power Watchdog are reachable ONLY as broadcasts
 * from the basement proxy, so a panel showing them needs this regardless of
 * having its own bus. Such a panel gets tank readings from both CAN and
 * these broadcasts; they carry the same numbers, so whichever arrives last
 * simply wins.
 */
static void remote_telem_rx(const espnow_telem_msg_t *msg, void *ctx)
{
    (void)ctx;
    switch (msg->kind) {
    case ESPNOW_TELEM_TANK:
        ui_on_tank_status(msg->tank.instance, msg->tank.percent,
                          msg->tank.valid != 0);
        break;

    case ESPNOW_TELEM_SHORE_POWER: {
        ui_shore_power_t sp = {
            .line_count   = msg->shore.line_count,
            .error_code   = msg->shore.error_code,
            .frequency_hz = (float)msg->shore.frequency_chz / 100.0f,
        };
        for (int i = 0; i < 2; i++) {
            sp.volts[i] = (float)msg->shore.volts_dv[i] / 10.0f;
            sp.amps[i]  = (float)msg->shore.amps_da[i] / 10.0f;
            sp.watts[i] = (float)msg->shore.watts[i];
        }
        ui_on_shore_power(&sp);
        break;
    }

    case ESPNOW_TELEM_BATTERY: {
        const espnow_battery_msg_t *b = &msg->battery;
        ui_battery_pack_t pack = {
            .slot             = b->slot,
            .online           = (b->flags & ESPNOW_BATTERY_FLAG_ONLINE) != 0,
            .soc_percent      = b->soc_percent,
            .voltage_v        = (float)b->volts_cv / 100.0f,
            .current_a        = (float)b->current_da / 10.0f,
            .residual_ah      = (float)b->residual_dah / 10.0f,
            .full_capacity_ah = (float)b->full_dah / 10.0f,
            .temp_valid       = (b->flags & ESPNOW_BATTERY_FLAG_TEMP_VALID) != 0,
            .temp_min_c       = (float)b->temp_min_dc / 10.0f,
            .temp_max_c       = (float)b->temp_max_dc / 10.0f,
        };
        ui_on_battery_status(&pack);
        break;
    }

    default:
        break;   /* newer producer, unknown measurement — ignore quietly */
    }
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

#if PANEL_HAS_SCREEN_2
#define TANK_TELEMETRY_PERIOD_MS 5000

/*
 * Broadcast this panel's tank readings so read-only remotes can show them.
 * Only the bridge sees TANK_STATUS frames — it is the one with CAN wiring —
 * so it is the natural producer.
 *
 * Which instances to send is taken from PANEL_BUTTONS_2 rather than
 * hardcoded here, the same trick ui.c uses to find the GREY/BLACK buttons:
 * a panel broadcasts exactly the tanks it displays, so adding a fourth
 * sensor is still a panel-header-only change.
 */
static void tank_telemetry_timer_cb(TimerHandle_t t)
{
    (void)t;
    for (uint32_t i = 0; i < PANEL_BUTTON_COUNT_2; i++) {
        const panel_btn_def_t *def = &PANEL_BUTTONS_2[i];
        if (def->type != PANEL_BTN_TANK_LEVEL || def->instance_count == 0) {
            continue;
        }
        uint8_t percent = 0;
        const bool valid = state_manager_get_tank(def->instances[0], &percent);

        espnow_telem_msg_t msg = { .kind = ESPNOW_TELEM_TANK };
        msg.tank.instance = def->instances[0];
        msg.tank.percent = percent;
        /* Send invalid readings too: a remote must be able to tell "no
         * reading" from "0 %", and silence alone can't distinguish a dry
         * tank from a dead bridge. */
        msg.tank.valid = valid ? 1u : 0u;
        espnow_link_send_telemetry(&msg);
    }
}
#endif
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
    ESP_ERROR_CHECK(espnow_link_init(ESPNOW_ROLE_BRIDGE));
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
#if PANEL_HAS_SCREEN_2
    TimerHandle_t tank_timer = xTimerCreate(
        "tank_telem", pdMS_TO_TICKS(TANK_TELEMETRY_PERIOD_MS), pdTRUE, NULL,
        tank_telemetry_timer_cb);
    if (tank_timer != NULL) {
        xTimerStart(tank_timer, 0);
    } else {
        ESP_LOGW(TAG, "failed to create tank telemetry timer");
    }
#endif
#elif !PANEL_HAS_CAN
    ESP_ERROR_CHECK(espnow_link_init(ESPNOW_ROLE_REMOTE));
    espnow_link_set_status_rx_cb(remote_status_rx, NULL);
    espnow_link_set_telem_rx_cb(remote_telem_rx, NULL);
#elif PANEL_WANTS_TELEMETRY
    /* Telemetry role = no unicast peer at all: this panel drives its own
     * loads over CAN and only LISTENS to the broadcast channel, for the
     * battery bank and shore power that live on the proxy's BLE links.
     * Nothing it receives here actuates anything. */
    ESP_ERROR_CHECK(espnow_link_init(ESPNOW_ROLE_TELEMETRY));
    espnow_link_set_telem_rx_cb(remote_telem_rx, NULL);
#endif

    ESP_LOGI(TAG, "up");
}
