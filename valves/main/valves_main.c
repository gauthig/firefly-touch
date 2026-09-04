/*
 * firefly-valves — headless dump-valve relay controller.
 *
 * A sixth node, separate from every panel: a Waveshare ESP32-S3-ETH-8DI-8RO
 * relay board in the basement bay, driving the two DrainMaster Premium dump
 * valves in parallel with the factory wall rockers. Full wiring, relay map,
 * and control requirements: docs/DRAINMASTER-VALVES.md.
 *
 * This scaffold covers bring-up order steps 1-3 (§10): TCA9554 relay
 * expander present at 0x20, individual relay clicks, the interlock
 * refusing a shorting mask, and the independent 2 s watchdog releasing
 * relays even when the driving code never calls release. It deliberately
 * stops there — ESP-NOW valve commands (and therefore anything that
 * actually drives a connected valve on request) are out of scope for this
 * PR; see the open items in DRAINMASTER-VALVES.md and the GitHub issue
 * this was scaffolded under.
 */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "driver/i2c_master.h"

#include "valve_control.h"
#include "valve_control_driver.h"

static const char *TAG = "valves";

/* I2C pins per DRAINMASTER-VALVES.md §7 "Board access": GPIO41 SCL /
 * GPIO42 SDA, shared with the onboard RTC. */
#define VALVE_I2C_GPIO_SCL  41
#define VALVE_I2C_GPIO_SDA  42

static i2c_master_bus_handle_t s_i2c_bus;

static esp_err_t i2c_bus_init(void)
{
    const i2c_master_bus_config_t cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .sda_io_num = VALVE_I2C_GPIO_SDA,
        .scl_io_num = VALVE_I2C_GPIO_SCL,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, &s_i2c_bus);
}

static void log_di_startup_state(void)
{
    /* Informational only until the sense circuit exists (§4) — a floating
     * DI pin reading either level here is expected, not a fault. This is
     * also the rule from §7 "Startup": read both inputs before any drive
     * and adopt that as the initial state, never assume closed. */
    ESP_LOGI(TAG, "DI startup read: grey=%s black=%s (uninformative until sense wiring exists)",
             valve_control_read_di(VALVE_GREY) ? "closed" : "not-closed",
             valve_control_read_di(VALVE_BLACK) ? "closed" : "not-closed");
}

#if CONFIG_FIREFLY_VALVE_TEST_MODE

static const char *relay_bit_name(uint8_t bit)
{
    switch (bit) {
    case VALVE_RELAY_GY_W_HI: return "CH1 (grey WHITE -> +12V)";
    case VALVE_RELAY_GY_W_LO: return "CH2 (grey WHITE -> gnd)";
    case VALVE_RELAY_GY_R_HI: return "CH3 (grey RED -> +12V)";
    case VALVE_RELAY_GY_R_LO: return "CH4 (grey RED -> gnd)";
    case VALVE_RELAY_BK_W_HI: return "CH5 (black WHITE -> +12V)";
    case VALVE_RELAY_BK_W_LO: return "CH6 (black WHITE -> gnd)";
    case VALVE_RELAY_BK_R_HI: return "CH7 (black RED -> +12V)";
    case VALVE_RELAY_BK_R_LO: return "CH8 (black RED -> gnd)";
    default: return "?";
    }
}

/* Bring-up step 1: click each relay individually, verifying the readback
 * matches what was commanded before moving to the next one. */
static void test_step1_individual_clicks(void)
{
    ESP_LOGI(TAG, "step 1: clicking each relay individually");
    for (uint8_t bit = 0; bit < 8; bit++) {
        const uint8_t mask = (uint8_t)(1u << bit);
        esp_err_t err = valve_control_apply(mask);
        vTaskDelay(pdMS_TO_TICKS(150));

        uint8_t readback = 0;
        esp_err_t rerr = valve_control_read_relays(&readback);
        bool ok = (err == ESP_OK) && (rerr == ESP_OK) && (readback == mask);
        ESP_LOGI(TAG, "  %s: commanded 0x%02x, read back 0x%02x -> %s",
                 relay_bit_name(mask), mask, readback, ok ? "OK" : "MISMATCH");

        valve_control_release_all();
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    ESP_LOGI(TAG, "step 1: done");
}

/* Bring-up step 2: a deliberately forbidden mask must be refused and must
 * drop the expander to all-off, not merely be logged. */
static void test_step2_interlock_refusal(void)
{
    ESP_LOGI(TAG, "step 2: interlock refusal test");
    const uint8_t forbidden = VALVE_RELAY_GY_W_HI | VALVE_RELAY_GY_W_LO;
    esp_err_t err = valve_control_apply(forbidden);

    uint8_t readback = 0xFFu;
    valve_control_read_relays(&readback);

    bool ok = (err == ESP_ERR_INVALID_ARG) && (readback == VALVE_MASK_ALL_OFF);
    ESP_LOGI(TAG, "step 2: forbidden mask 0x%02x -> apply()=%s, relays read 0x%02x -> %s",
             forbidden, esp_err_to_name(err), readback, ok ? "PASS" : "FAIL");
}

/* Bring-up step 3: the independent watchdog must release the relays even
 * when the caller never calls valve_control_release_all() itself. This is
 * "the test that matters most" per the doc — everything downstream of it
 * assumes the ceiling is real. */
static void test_step3_watchdog(void)
{
    ESP_LOGI(TAG, "step 3: watchdog test — driving CH1 and NOT releasing it");
    valve_control_apply(VALVE_RELAY_GY_W_HI);

    vTaskDelay(pdMS_TO_TICKS(VALVE_DRIVE_CEILING_MS + 500));

    uint8_t readback = 0xFFu;
    esp_err_t rerr = valve_control_read_relays(&readback);
    bool ok = (rerr == ESP_OK) && (readback == VALVE_MASK_ALL_OFF);
    ESP_LOGI(TAG, "step 3: %u ms after drive (never released by caller), relays read 0x%02x -> %s",
             (unsigned)(VALVE_DRIVE_CEILING_MS + 500), readback, ok ? "PASS" : "FAIL");
}

static void run_test_mode(void)
{
    ESP_LOGW(TAG, "VALVE TEST MODE — no valve or sense wiring required for this sequence");
    test_step1_individual_clicks();
    test_step2_interlock_refusal();
    test_step3_watchdog();
    ESP_LOGW(TAG, "VALVE TEST MODE complete — see DRAINMASTER-VALVES.md #10 for the remaining steps");
}

#endif /* CONFIG_FIREFLY_VALVE_TEST_MODE */

void app_main(void)
{
    ESP_LOGI(TAG, "firefly-valves starting");

    ESP_ERROR_CHECK(i2c_bus_init());
    ESP_ERROR_CHECK(valve_control_driver_init(s_i2c_bus));

    log_di_startup_state();

#if CONFIG_FIREFLY_VALVE_TEST_MODE
    run_test_mode();
#else
    ESP_LOGI(TAG, "test mode disabled; idle (no ESP-NOW command path yet)");
#endif

    ESP_LOGI(TAG, "up");
}
