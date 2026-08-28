/*
 * twai_tasks — RV-C bus I/O, core 0.
 *
 *   twai_rx_task  (prio 12): blocks on twai_receive(), stamps bus liveness,
 *                            optional sniffer logging, decodes
 *                            DC_DIMMER_STATUS_3 and posts to the status
 *                            queue for the state manager.
 *   twai_tx_task  (prio 11): drains the TX queue and transmits, so nothing
 *                            else ever blocks on the bus. Handles bus-off
 *                            recovery.
 */
#include "twai_tasks.h"

#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "driver/twai.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "panel_config.h"
#include "rvc_protocol.h"
#include "state_manager.h"

static const char *TAG = "twai_tasks";

#define TWAI_TASK_CORE     0
#define RX_TASK_PRIO       12
#define TX_TASK_PRIO       11
#define RX_TASK_STACK      4096
#define TX_TASK_STACK      4096

#define TX_QUEUE_LEN       16
#define STATUS_QUEUE_LEN   32
#define TANK_QUEUE_LEN     8

static QueueHandle_t s_tx_queue;
static QueueHandle_t s_status_queue;
static QueueHandle_t s_tank_status_queue;

/* ------------------------------------------------------------------ RX -- */

static void twai_rx_task(void *arg)
{
    (void)arg;
    twai_message_t msg;

    for (;;) {
        if (twai_receive(&msg, portMAX_DELAY) != ESP_OK) {
            continue;
        }

        state_manager_note_rx();

        if (!msg.extd) {
            continue;   /* RV-C uses 29-bit IDs exclusively */
        }

        const rvc_id_t id = rvc_id_unpack(msg.identifier);

#if CONFIG_FIREFLY_SNIFFER_MODE
        ESP_LOGI(TAG,
                 "RX id=0x%08" PRIX32 " dgn=0x%05" PRIX32 " sa=0x%02X dlc=%u "
                 "data=%02X %02X %02X %02X %02X %02X %02X %02X",
                 msg.identifier, id.dgn, id.source_addr, msg.data_length_code,
                 msg.data[0], msg.data[1], msg.data[2], msg.data[3],
                 msg.data[4], msg.data[5], msg.data[6], msg.data[7]);
#endif

        if (id.dgn == RVC_DGN_DC_DIMMER_STATUS_3) {
            rvc_dimmer_status_t st;
            if (rvc_decode_dc_dimmer_status_3(msg.data, msg.data_length_code, &st)) {
                const dimmer_status_msg_t out = {
                    .instance = st.instance,
                    .level = st.operating_level,
                    .on = st.load_on,
                };
                if (xQueueSend(s_status_queue, &out, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "status queue full, dropped instance %u", st.instance);
                }
            }
        } else if (id.dgn == RVC_DGN_TANK_STATUS) {
            rvc_tank_status_t st;
            if (rvc_decode_tank_status(msg.data, msg.data_length_code, &st)) {
                const tank_status_msg_t out = {
                    .instance = st.instance,
                    .percent = st.percent,
                    .valid = st.valid,
                };
                if (xQueueSend(s_tank_status_queue, &out, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "tank queue full, dropped instance %u", st.instance);
                }
            }
        }
        /* Other status DGNs (e.g. the PL1 panel-light DGN once identified)
         * get filtered and decoded here. */
    }
}

/* ------------------------------------------------------------------ TX -- */

static void recover_if_bus_off(void)
{
    twai_status_info_t info;
    if (twai_get_status_info(&info) == ESP_OK && info.state == TWAI_STATE_BUS_OFF) {
        ESP_LOGE(TAG, "bus-off — initiating recovery");
        twai_initiate_recovery();
        /* Driver returns to stopped state once recovered. */
        vTaskDelay(pdMS_TO_TICKS(100));
        twai_start();
    }
}

static void twai_tx_task(void *arg)
{
    (void)arg;
    dimmer_cmd_msg_t cmd;

    for (;;) {
        if (xQueueReceive(s_tx_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        const rvc_dimmer_command_t rc = {
            .instance = cmd.instance,
            .group = cmd.group,
            .level = cmd.level,
            .command = cmd.command,
            .duration = cmd.duration,
        };

        twai_message_t msg = {
            .identifier = rvc_id_pack(RVC_PRIORITY_DEFAULT,
                                      RVC_DGN_DC_DIMMER_COMMAND_2,
                                      PANEL_SOURCE_ADDR),
            .extd = 1,
            .data_length_code = 8,
        };
        rvc_encode_dc_dimmer_command_2(&rc, msg.data);

        const esp_err_t err = twai_transmit(&msg, pdMS_TO_TICKS(100));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "TX failed (%s): inst=%u cmd=%u",
                     esp_err_to_name(err), cmd.instance, cmd.command);
            recover_if_bus_off();
        } else {
            ESP_LOGD(TAG, "TX inst=%u cmd=%u level=%u",
                     cmd.instance, cmd.command, cmd.level);
        }
    }
}

/* ----------------------------------------------------------------- API -- */

bool twai_enqueue_dimmer_cmd(uint8_t instance, rvc_dimmer_cmd_t cmd,
                             uint8_t level, uint8_t duration)
{
    if (s_tx_queue == NULL) {
        return false;
    }
    const dimmer_cmd_msg_t msg = {
        .instance = instance,
        .command = cmd,
        .level = level,
        .duration = duration,
        .group = RVC_FIELD_NA,   /* instance-addressed: no group */
    };
    if (xQueueSend(s_tx_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "TX queue full, dropped cmd %u for instance %u", cmd, instance);
        return false;
    }
    return true;
}

bool twai_enqueue_dimmer_group_cmd(uint8_t group, rvc_dimmer_cmd_t cmd,
                                   uint8_t level)
{
    if (s_tx_queue == NULL) {
        return false;
    }
    const dimmer_cmd_msg_t msg = {
        .instance = RVC_INSTANCE_ALL,
        .command = cmd,
        .level = level,
        .duration = RVC_FIELD_NA,
        .group = group,
    };
    if (xQueueSend(s_tx_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "TX queue full, dropped cmd %u for group 0x%02X", cmd, group);
        return false;
    }
    return true;
}

esp_err_t twai_tasks_start(void)
{
    s_tx_queue = xQueueCreate(TX_QUEUE_LEN, sizeof(dimmer_cmd_msg_t));
    s_status_queue = xQueueCreate(STATUS_QUEUE_LEN, sizeof(dimmer_status_msg_t));
    s_tank_status_queue = xQueueCreate(TANK_QUEUE_LEN, sizeof(tank_status_msg_t));
    if (s_tx_queue == NULL || s_status_queue == NULL || s_tank_status_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(state_manager_start(s_status_queue));
    ESP_ERROR_CHECK(state_manager_start_tanks(s_tank_status_queue));

    BaseType_t ok;
    ok = xTaskCreatePinnedToCore(twai_rx_task, "twai_rx", RX_TASK_STACK, NULL,
                                 RX_TASK_PRIO, NULL, TWAI_TASK_CORE);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ok = xTaskCreatePinnedToCore(twai_tx_task, "twai_tx", TX_TASK_STACK, NULL,
                                 TX_TASK_PRIO, NULL, TWAI_TASK_CORE);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "RX/TX tasks running on core %d, source addr 0x%02X",
             TWAI_TASK_CORE, PANEL_SOURCE_ADDR);
    return ESP_OK;
}
