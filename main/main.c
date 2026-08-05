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
#include "esp_ota_ops.h"

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

    /*
     * Everything came up: display, touch, UI, and the RV-C tasks. Confirm the
     * image so the bootloader stops treating it as provisional.
     *
     * With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE, an image installed over OTA
     * boots as PENDING_VERIFY and is rolled back to the previous slot on the
     * next reset unless this call happens. A panel that can't bring up its
     * display or CAN bus therefore reverts on its own instead of needing to be
     * pulled out of the wall. No-op for USB-flashed images.
     */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
        ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "OTA image healthy — marking valid, cancelling rollback");
        esp_ota_mark_app_valid_cancel_rollback();
    }

    ESP_LOGI(TAG, "up — waiting for DC_DIMMER_STATUS_3 traffic");
}
