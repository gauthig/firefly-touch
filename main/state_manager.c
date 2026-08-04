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
