/*
 * state_manager — owns the instance -> state table, core 0.
 *
 * The ONLY writer of button visual state: consumes decoded
 * DC_DIMMER_STATUS_3 messages and notifies the UI on change. Commands never
 * update state locally — a light is "on" when the bus says it is, which
 * keeps every panel consistent with the factory switches and the Firefly
 * app.
 */
#include "state_manager.h"

#include <string.h>

#include "freertos/task.h"

#include "esp_log.h"

#include "app_msgs.h"
#include "ui.h"

static const char *TAG = "state_mgr";

#define STATE_TASK_CORE   0
#define STATE_TASK_PRIO   9
#define STATE_TASK_STACK  6144   /* calls into LVGL (under lock) for UI updates */

#define BUS_HEALTHY_WINDOW_MS 5000

typedef struct {
    uint8_t level;
    bool    on;
    bool    known;
} instance_state_t;

static instance_state_t s_states[256];
/* 32-bit aligned tick write/read is atomic on Xtensa; no lock needed. */
static volatile TickType_t s_last_rx_tick;
static volatile bool s_rx_seen;

static state_status_sink_t s_sink;
static void *s_sink_ctx;

void state_manager_register_status_sink(state_status_sink_t cb, void *ctx)
{
    s_sink = cb;
    s_sink_ctx = ctx;
}

void state_manager_for_each_known(state_status_sink_t cb, void *ctx)
{
    if (cb == NULL) {
        return;
    }
    for (uint32_t i = 0; i < 256; i++) {
        if (s_states[i].known) {
            cb((uint8_t)i, s_states[i].level, s_states[i].on, ctx);
        }
    }
}

void state_manager_note_rx(void)
{
    s_last_rx_tick = xTaskGetTickCount();
    s_rx_seen = true;
}

bool state_manager_bus_healthy(void)
{
    if (!s_rx_seen) {
        return false;
    }
    return (xTaskGetTickCount() - s_last_rx_tick) < pdMS_TO_TICKS(BUS_HEALTHY_WINDOW_MS);
}

bool state_manager_get(uint8_t instance, uint8_t *level, bool *on)
{
    const instance_state_t st = s_states[instance];
    if (!st.known) {
        return false;
    }
    if (level != NULL) {
        *level = st.level;
    }
    if (on != NULL) {
        *on = st.on;
    }
    return true;
}

static void state_manager_task(void *arg)
{
    QueueHandle_t queue = arg;
    dimmer_status_msg_t msg;

    for (;;) {
        if (xQueueReceive(queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        instance_state_t *st = &s_states[msg.instance];
        const bool changed = !st->known || st->level != msg.level || st->on != msg.on;
        st->level = msg.level;
        st->on = msg.on;
        st->known = true;

        if (changed) {
            ESP_LOGD(TAG, "instance %u -> level=%u on=%d", msg.instance, msg.level, msg.on);
            ui_on_status(msg.instance, msg.level, msg.on);
            if (s_sink != NULL) {
                s_sink(msg.instance, msg.level, msg.on, s_sink_ctx);
            }
        }
    }
}

esp_err_t state_manager_start(QueueHandle_t status_queue)
{
    memset((void *)s_states, 0, sizeof(s_states));
    const BaseType_t ok = xTaskCreatePinnedToCore(
        state_manager_task, "state_mgr", STATE_TASK_STACK, status_queue,
        STATE_TASK_PRIO, NULL, STATE_TASK_CORE);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

/* ------------------------------------------------------- tank sensors --- */

/* Covers instance 0-19 (the full range documented in
 * docs/instance_map.yaml -> tank_dgn) with a little margin; deliberately
 * NOT the 256-entry dimmer table above -- different DGN, different
 * namespace, must never collide with a dimmer instance number. */
#define TANK_INSTANCE_SLOTS 32

typedef struct {
    uint8_t percent;
    bool    valid;
    bool    known;
} tank_state_t;

static tank_state_t s_tank_states[TANK_INSTANCE_SLOTS];

bool state_manager_get_tank(uint8_t instance, uint8_t *percent)
{
    if (instance >= TANK_INSTANCE_SLOTS) {
        return false;
    }
    const tank_state_t st = s_tank_states[instance];
    if (!st.known) {
        return false;
    }
    if (percent != NULL) {
        *percent = st.percent;
    }
    return st.valid;
}

static void tank_state_task(void *arg)
{
    QueueHandle_t queue = arg;
    tank_status_msg_t msg;

    for (;;) {
        if (xQueueReceive(queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (msg.instance >= TANK_INSTANCE_SLOTS) {
            ESP_LOGW(TAG, "tank instance %u out of range, dropped", msg.instance);
            continue;
        }

        tank_state_t *st = &s_tank_states[msg.instance];
        const bool changed = !st->known || st->percent != msg.percent || st->valid != msg.valid;
        st->percent = msg.percent;
        st->valid = msg.valid;
        st->known = true;

        if (changed) {
            ESP_LOGD(TAG, "tank instance %u -> percent=%u valid=%d",
                     msg.instance, msg.percent, msg.valid);
            ui_on_tank_status(msg.instance, msg.percent, msg.valid);
        }
    }
}

esp_err_t state_manager_start_tanks(QueueHandle_t tank_status_queue)
{
    memset(s_tank_states, 0, sizeof(s_tank_states));
    const BaseType_t ok = xTaskCreatePinnedToCore(
        tank_state_task, "tank_mgr", STATE_TASK_STACK, tank_status_queue,
        STATE_TASK_PRIO, NULL, STATE_TASK_CORE);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
