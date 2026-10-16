#include "valve_drive.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "driver/gpio.h"
#include "esp_log.h"

#include "espnow_link.h"
#include "valve_control.h"
#include "valve_control_driver.h"

static const char *TAG = "valve_drive";

#define CMD_QUEUE_LEN    4
#define DRIVE_TASK_STACK 4096
#define DRIVE_TASK_PRIO  5

/* True when the DI sense circuits drive what the panel is told. The sensor
 * test build turns it on regardless of FIREFLY_VALVE_SENSE_ENABLED so the
 * panel can be watched tracking the valve, while still never driving one. */
#define VALVE_SENSE_LIVE (CONFIG_FIREFLY_VALVE_SENSE_ENABLED || CONFIG_FIREFLY_VALVE_SENSE_TEST)

#define SENSE_POLL_MS       25
#define SENSE_SETTLE_MS     400    /* after a drive ends, before the sensor is trusted again */
#define SENSE_HEARTBEAT_MS  2000

static QueueHandle_t s_cmd_queue;
static uint8_t s_position[VALVE_COUNT];          /* espnow_valve_position_t */
static TickType_t s_lockout_until[VALVE_COUNT];  /* 0 = not locked out */
static volatile bool s_driving;                  /* a drive is in progress */
static TickType_t s_settle_until;                /* sensor ignored until then */
static bool s_close_fault[VALVE_COUNT];          /* a CLOSE failed to reach the sensor */

static void send_status(valve_id_t valve);

static void valve_cmd_rx(const espnow_valve_cmd_msg_t *msg, void *ctx)
{
    (void)ctx;
#if CONFIG_FIREFLY_VALVE_SENSE_TEST
    /* Read-only build: no relay may ever be energized from here. */
    ESP_LOGW(TAG, "SENSE TEST build: dropped valve %u action %u (no relays are driven)",
             (unsigned)msg->valve, (unsigned)msg->action);
    return;
#endif
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
    s_driving = true;   /* the sensor task must not second-guess a drive in flight */
    valve_control_release_all();
    vTaskDelay(pdMS_TO_TICKS(VALVE_BREAK_BEFORE_MAKE_MS));

    const uint8_t mask = valve_drive_mask(valve, action);
    if (valve_control_apply(mask) != ESP_OK) {
        ESP_LOGE(TAG, "valve %u: relay apply failed, aborting drive", (unsigned)valve);
        s_position[valve] = ESPNOW_VALVE_POS_UNKNOWN;
        s_settle_until = xTaskGetTickCount() + pdMS_TO_TICKS(SENSE_SETTLE_MS);
        s_driving = false;
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
    /* A CLOSE that never reached the sensor is a latched fault, not just
     * "not closed": the sensor task keeps reporting UNKNOWN until the valve
     * is seen closed, so the failure is not silently relabelled OPEN. */
    s_close_fault[valve] = (action == VALVE_ACTION_CLOSE) &&
                           (s_position[valve] == ESPNOW_VALVE_POS_UNKNOWN);
    s_settle_until = xTaskGetTickCount() + pdMS_TO_TICKS(SENSE_SETTLE_MS);
    s_driving = false;

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

#if CONFIG_FIREFLY_VALVE_SENSE_ENABLED
        /* A CLOSE for a valve the sensor already reads closed energizes
         * nothing: the panel only sends CLOSE for a valve that reads open or
         * unknown, so this is the race where it was stale. Re-report the true
         * position so the panel catches up.
         *
         * Deliberately NOT done for OPEN. "Not closed" only means the reed has
         * left its closed position, which happens EARLY in the stroke -- a
         * valve reading not-closed may be mid-travel, and a repeated OPEN is
         * how it finishes opening. */
        if (action == VALVE_ACTION_CLOSE && valve_control_read_di(valve)) {
            ESP_LOGI(TAG, "valve %u: CLOSE ignored, sensor already reads closed",
                     (unsigned)valve);
            s_close_fault[valve] = false;
            s_position[valve] = ESPNOW_VALVE_POS_CLOSED;
            send_status(valve);
            continue;
        }
#endif
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

/*
 * Sensor task. Polls DI1/DI2, logs every raw level change (this is the
 * sensor test: polarity and bounce are measured, not assumed), and -- when
 * the sense circuit drives reporting -- keeps the reported position equal
 * to the sensor so a valve moved by the WALL ROCKER shows up on the panel
 * too, not only one moved by a panel command.
 *
 * Sensor says closed          -> CLOSED
 * Sensor says not closed      -> OPEN ("not closed": the reed flips early in
 *                                the open stroke, so this is not full-open)
 * ...unless a CLOSE failed    -> UNKNOWN until the valve is seen closed
 *
 * Skipped while a drive is in flight and for SENSE_SETTLE_MS after it, so
 * the drive's own result is reported first.
 */
static void valve_sense_task(void *arg)
{
    (void)arg;
    bool last_raw[VALVE_COUNT];
    bool stable_closed[VALVE_COUNT];
    TickType_t changed_at[VALVE_COUNT];
    TickType_t last_beat = xTaskGetTickCount();

    for (valve_id_t v = 0; v < VALVE_COUNT; v++) {
        last_raw[v] = valve_control_read_di_raw(v);
        stable_closed[v] = valve_control_read_di(v);
        changed_at[v] = xTaskGetTickCount();
        ESP_LOGI(TAG, "valve %u sensor: initial raw=%d -> %s", (unsigned)v,
                 (int)last_raw[v], stable_closed[v] ? "CLOSED" : "NOT closed");
    }

#if CONFIG_FIREFLY_VALVE_SENSE_TEST
    /* Pin-mapping diagnostic: watch ALL eight DI GPIOs (DI1..DI8 = GPIO4..11
     * per the vendor wiki). If DI1/DI2 never move but another pin does, the
     * wiki mapping is wrong for this board. GPIO4/5 are already configured
     * by the driver; configure the rest as plain inputs. */
    const gpio_config_t scan_cfg = {
        .pin_bit_mask = 0xFC0ULL,   /* GPIO6..11 */
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&scan_cfg);
    uint8_t scan_last = 0xFF;
    TickType_t scan_changed_at = xTaskGetTickCount();
    uint8_t scan_logged = 0xFE;
#endif

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SENSE_POLL_MS));
        const TickType_t now = xTaskGetTickCount();

#if CONFIG_FIREFLY_VALVE_SENSE_TEST
        uint8_t scan = 0;
        for (int i = 0; i < 8; i++) {
            scan |= (uint8_t)(gpio_get_level((gpio_num_t)(4 + i)) ? (1u << i) : 0u);
        }
        if (scan != scan_last) {
            scan_last = scan;
            scan_changed_at = now;
        }
        if (scan != scan_logged && (now - scan_changed_at) >= pdMS_TO_TICKS(VALVE_SENSE_DEBOUNCE_MS)) {
            scan_logged = scan;
            ESP_LOGI(TAG, "DI scan GPIO4..11 (bit0=GPIO4=DI1): 0x%02x  DI1=%d DI2=%d DI3=%d DI4=%d DI5=%d DI6=%d DI7=%d DI8=%d",
                     scan, scan & 1, (scan >> 1) & 1, (scan >> 2) & 1, (scan >> 3) & 1,
                     (scan >> 4) & 1, (scan >> 5) & 1, (scan >> 6) & 1, (scan >> 7) & 1);
        }
#endif

        for (valve_id_t v = 0; v < VALVE_COUNT; v++) {
            const bool raw = valve_control_read_di_raw(v);
            if (raw != last_raw[v]) {
                last_raw[v] = raw;
                changed_at[v] = now;
                ESP_LOGI(TAG, "valve %u sensor: raw=%d (settling)", (unsigned)v, (int)raw);
            }
            const bool closed = valve_control_read_di(v);
            if (closed != stable_closed[v] &&
                (now - changed_at[v]) >= pdMS_TO_TICKS(VALVE_SENSE_DEBOUNCE_MS)) {
                stable_closed[v] = closed;
                ESP_LOGI(TAG, "valve %u sensor: stable -> %s", (unsigned)v,
                         closed ? "CLOSED" : "NOT closed");
            }

#if VALVE_SENSE_LIVE
            if (!s_driving && now >= s_settle_until) {
                if (stable_closed[v]) {
                    s_close_fault[v] = false;
                }
                const uint8_t want = stable_closed[v] ? ESPNOW_VALVE_POS_CLOSED
                                   : s_close_fault[v] ? ESPNOW_VALVE_POS_UNKNOWN
                                                      : ESPNOW_VALVE_POS_OPEN;
                if (want != s_position[v]) {
                    ESP_LOGI(TAG, "valve %u: sensor moves reported position %s -> %s",
                             (unsigned)v, position_str(s_position[v]), position_str(want));
                    s_position[v] = want;
                    send_status(v);
                }
            }
#endif
        }

        if ((now - last_beat) >= pdMS_TO_TICKS(SENSE_HEARTBEAT_MS)) {
            last_beat = now;
            ESP_LOGI(TAG, "DI1(grey) raw=%d %s | DI2(black) raw=%d %s",
                     (int)last_raw[VALVE_GREY], stable_closed[VALVE_GREY] ? "CLOSED" : "NOT closed",
                     (int)last_raw[VALVE_BLACK], stable_closed[VALVE_BLACK] ? "CLOSED" : "NOT closed");
        }
    }
}

esp_err_t valve_drive_init(void)
{
    s_cmd_queue = xQueueCreate(CMD_QUEUE_LEN, sizeof(espnow_valve_cmd_msg_t));
    if (s_cmd_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Startup position.
     *
     * With the sense circuit wired: read both DIs and adopt that -- closed
     * seeds CLOSED, not-closed seeds OPEN. Not-closed IS real evidence (the
     * reed leaves its closed position early in the stroke), unlike guessing,
     * and showing it keeps an open valve visible after a reboot.
     *
     * Without it (the temp process until the sense circuits exist -- see
     * CONFIG_FIREFLY_VALVE_SENSE_ENABLED): **assume CLOSED.** The valves
     * are left closed after every dump, so closed is the safe and almost
     * always correct default, and it means a first tap on a panel button
     * goes straight to the arm-then-fire OPEN path instead of sitting on
     * "unknown". A stray reboot with a valve actually open still recovers
     * -- the panel's single-tap CLOSE runs a timed close drive regardless
     * of the reported position. */
    for (valve_id_t v = 0; v < VALVE_COUNT; v++) {
#if VALVE_SENSE_LIVE
        s_position[v] = valve_control_read_di(v) ? ESPNOW_VALVE_POS_CLOSED
                                                  : ESPNOW_VALVE_POS_OPEN;
#else
        s_position[v] = ESPNOW_VALVE_POS_CLOSED;
#endif
        ESP_LOGI(TAG, "valve %u: startup position %s", (unsigned)v,
                 position_str(s_position[v]));
    }

    espnow_link_set_valve_cmd_rx_cb(valve_cmd_rx, NULL);

    if (xTaskCreate(valve_drive_task, "valve_drive", DRIVE_TASK_STACK, NULL,
                    DRIVE_TASK_PRIO, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(valve_sense_task, "valve_sense", 3072, NULL,
                    DRIVE_TASK_PRIO - 1, NULL) != pdPASS) {
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
