#include "valve_drive.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "esp_log.h"

#include "espnow_link.h"
#include "valve_control.h"
#include "valve_control_driver.h"

static const char *TAG = "valve_drive";

#define CMD_QUEUE_LEN    4
#define DRIVE_TASK_STACK 4096
#define DRIVE_TASK_PRIO  5

static QueueHandle_t s_cmd_queue;
static uint8_t s_position[VALVE_COUNT];          /* espnow_valve_position_t */
static TickType_t s_lockout_until[VALVE_COUNT];  /* 0 = not locked out */

static void valve_cmd_rx(const espnow_valve_cmd_msg_t *msg, void *ctx)
{
    (void)ctx;
    /* Runs in espnow_rx_task context -- never blocks. A full queue means
     * commands are arriving faster than they can be driven, which the
     * re-drive lockout below should already be preventing; drop and log
     * rather than pdMS_TO_TICKS(anything). */
    if (xQueueSend(s_cmd_queue, msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "cmd queue full, dropped valve %u action %u",
                 (unsigned)msg->valve, (unsigned)msg->action);
    }
}

static const char *position_str(uint8_t position)
{
    switch (position) {
    case ESPNOW_VALVE_POS_CLOSED: return "closed";
    case ESPNOW_VALVE_POS_OPEN:   return "open (timed)";
    default:                      return "unknown";
    }
}

static void send_status(valve_id_t valve)
{
    const espnow_valve_status_msg_t status = {
        .valve = (uint8_t)valve,
        .position = s_position[valve],
    };
    espnow_link_send_valve_status(&status);
}

/*
 * Runs one full drive to completion: break-before-make, drive, poll
 * valve_decide_step() until it says stop, release, record + report the
 * result. Never returns early -- a command for the other valve simply
 * waits in the queue, which is what serializes drives without a separate
 * busy flag.
 */
static void run_drive(valve_id_t valve, valve_action_t action)
{
    ESP_LOGI(TAG, "valve %u: %s starting", (unsigned)valve,
             action == VALVE_ACTION_OPEN ? "OPEN" : "CLOSE");

    /* Rule 2: always drop to all-off and wait before energizing a new
     * pair, even if the previous drive already released cleanly -- this is
     * the one choke point's other half, not just an optimization. */
    valve_control_release_all();
    vTaskDelay(pdMS_TO_TICKS(VALVE_BREAK_BEFORE_MAKE_MS));

    const uint8_t mask = valve_drive_mask(valve, action);
    if (valve_control_apply(mask) != ESP_OK) {
        ESP_LOGE(TAG, "valve %u: relay apply failed, aborting drive", (unsigned)valve);
        s_position[valve] = ESPNOW_VALVE_POS_UNKNOWN;
        send_status(valve);
        return;
    }

    const TickType_t start = xTaskGetTickCount();
    valve_step_result_t result;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(VALVE_SENSE_DEBOUNCE_MS));
        const uint32_t elapsed_ms =
            (uint32_t)((xTaskGetTickCount() - start) * portTICK_PERIOD_MS);
#if CONFIG_FIREFLY_VALVE_SENSE_ENABLED
        const bool di_closed = (action == VALVE_ACTION_CLOSE) &&
                               valve_control_read_di(valve);
#else
        /* No sense circuit wired yet (docs/DRAINMASTER-VALVES.md #4,
         * blocked on a missing capacitor) -- synthesize "confirmed" at the
         * nominal drive time instead of polling a floating DI pin, which
         * bench-tested 2026-09-04 as either never confirming (timing out
         * to unknown every close) or floating "closed" from boot and
         * confirming almost instantly -- neither reflects anything real.
         * This makes CLOSE a timed 1 s drive exactly like OPEN, same
         * VALVE_DRIVE_NOMINAL_MS, until the real circuit exists. */
        const bool di_closed = (action == VALVE_ACTION_CLOSE) &&
                               (elapsed_ms >= VALVE_DRIVE_NOMINAL_MS);
#endif
        result = valve_decide_step(action, elapsed_ms, di_closed);
        if (result != VALVE_STEP_CONTINUE) {
            break;
        }
    }

    /* Idempotent even if the independent watchdog already released the
     * relays (the VALVE_STEP_RELEASE_UNKNOWN path) -- see
     * valve_control_apply()'s own re-arm-on-every-call behavior. */
    valve_control_release_all();

    switch (result) {
    case VALVE_STEP_RELEASE_CLOSED:  s_position[valve] = ESPNOW_VALVE_POS_CLOSED; break;
    case VALVE_STEP_RELEASE_OPEN:    s_position[valve] = ESPNOW_VALVE_POS_OPEN; break;
    default:                         s_position[valve] = ESPNOW_VALVE_POS_UNKNOWN; break;
    }
    s_lockout_until[valve] = xTaskGetTickCount() + pdMS_TO_TICKS(VALVE_REDRIVE_LOCKOUT_MS);

    ESP_LOGI(TAG, "valve %u: %s done -> %s", (unsigned)valve,
             action == VALVE_ACTION_OPEN ? "OPEN" : "CLOSE",
             position_str(s_position[valve]));
    send_status(valve);
}

static void valve_drive_task(void *arg)
{
    (void)arg;
    espnow_valve_cmd_msg_t cmd;

    for (;;) {
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (cmd.valve >= VALVE_COUNT) {
            ESP_LOGW(TAG, "ignoring command for unknown valve %u", (unsigned)cmd.valve);
            continue;
        }

        const valve_id_t valve = (valve_id_t)cmd.valve;
        if (xTaskGetTickCount() < s_lockout_until[valve]) {
            /* Rule: "stops command spam stalling the motor" -- dropped, not
             * queued for later. A command that arrives during lockout is
             * gone, not deferred; the user (or panel) is expected to tap
             * again once it clears. */
            ESP_LOGW(TAG, "valve %u: command dropped, still in re-drive lockout",
                     (unsigned)valve);
            continue;
        }

        const valve_action_t action = (cmd.action == ESPNOW_VALVE_ACTION_OPEN)
            ? VALVE_ACTION_OPEN : VALVE_ACTION_CLOSE;
        run_drive(valve, action);
    }
}

/* Periodic resync, same self-healing reasoning as mid_coach's own
 * bridge_resync_timer_cb (main/main.c): status broadcasts are best-effort
 * with no ack, so a panel that just booted or missed one gets corrected by
 * the next periodic resend rather than staying stale until the next real
 * state change. */
static void resync_timer_cb(TimerHandle_t t)
{
    (void)t;
    for (valve_id_t v = 0; v < VALVE_COUNT; v++) {
        send_status(v);
    }
}

esp_err_t valve_drive_init(void)
{
    s_cmd_queue = xQueueCreate(CMD_QUEUE_LEN, sizeof(espnow_valve_cmd_msg_t));
    if (s_cmd_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Startup: read both inputs before any drive and adopt that as the
     * initial state -- never assume closed. A DI reading closed seeds
     * CLOSED; anything else seeds UNKNOWN rather than guessing OPEN, since
     * OPEN is never sensor-confirmable anyway.
     *
     * With no sense circuit wired, a DI read here is just floating-pin
     * noise (see CONFIG_FIREFLY_VALVE_SENSE_ENABLED's help), so both
     * valves simply start unknown rather than trusting it. */
    for (valve_id_t v = 0; v < VALVE_COUNT; v++) {
#if CONFIG_FIREFLY_VALVE_SENSE_ENABLED
        s_position[v] = valve_control_read_di(v) ? ESPNOW_VALVE_POS_CLOSED
                                                  : ESPNOW_VALVE_POS_UNKNOWN;
#else
        s_position[v] = ESPNOW_VALVE_POS_UNKNOWN;
#endif
        ESP_LOGI(TAG, "valve %u: startup position %s", (unsigned)v,
                 position_str(s_position[v]));
    }

    espnow_link_set_valve_cmd_rx_cb(valve_cmd_rx, NULL);

    if (xTaskCreate(valve_drive_task, "valve_drive", DRIVE_TASK_STACK, NULL,
                    DRIVE_TASK_PRIO, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    TimerHandle_t resync = xTimerCreate(
        "valve_resync", pdMS_TO_TICKS(CONFIG_FIREFLY_VALVE_RESYNC_INTERVAL_MS),
        pdTRUE, NULL, resync_timer_cb);
    if (resync == NULL) {
        ESP_LOGW(TAG, "failed to create resync timer");
    } else {
        xTimerStart(resync, 0);
    }

    return ESP_OK;
}
