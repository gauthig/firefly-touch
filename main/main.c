/*
 * firefly-touch — RV-C touchscreen wall panel for a Firefly G6A multiplex
 * coach. Each panel is an independent peer node on the 250 kbps RV-C bus;
 * there is no hub, the bus is the shared state.
 *
 * Task map:
 *   core 0: twai_rx_task (12) / twai_tx_task (11) / state_mgr (9)
 *   core 1: LVGL task via esp_lvgl_port (4)
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "board_4_3b.h"
#include "panel_config.h"
#include "twai_tasks.h"
#include "ui.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "firefly-touch panel '%s' (index %d, RV-C source addr 0x%02X)",
             PANEL_NAME, PANEL_INDEX, PANEL_SOURCE_ADDR);

    /* Display + touch + LVGL (core 1). */
    ESP_ERROR_CHECK(board_display_init());

    /* Build the screen before bus traffic starts flowing so the first
     * status frames land on live widgets. */
    ui_init();

    /* CAN driver, then the protocol tasks (core 0). */
    ESP_ERROR_CHECK(board_twai_init());
    ESP_ERROR_CHECK(twai_tasks_start());

    ESP_LOGI(TAG, "up — waiting for DC_DIMMER_STATUS_3 traffic");
}
