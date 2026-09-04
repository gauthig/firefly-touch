#include "valve_control_driver.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "valve_control";

/* TCA9554 register-pointer model (unlike CH422G's fixed-address-per-
 * register scheme): one device address, four registers selected by a
 * pointer byte sent before each access. */
#define TCA9554_I2C_ADDR       0x20u
#define TCA9554_REG_INPUT      0x00u
#define TCA9554_REG_OUTPUT     0x01u
#define TCA9554_REG_POLARITY   0x02u
#define TCA9554_REG_CONFIG     0x03u  /* bit=1 -> input (chip default: all input) */

/* DI GPIOs per DRAINMASTER-VALVES.md §7 "Board access": DI1 = GPIO4
 * (grey), DI2 = GPIO5 (black). DI3-DI8 are unused by this design. */
#define VALVE_DI_GPIO_GREY   GPIO_NUM_4
#define VALVE_DI_GPIO_BLACK  GPIO_NUM_5

static i2c_master_dev_handle_t s_tca9554;
static esp_timer_handle_t s_watchdog;

static esp_err_t tca9554_write_reg(uint8_t reg, uint8_t value)
{
    const uint8_t buf[2] = { reg, value };
    return i2c_master_transmit(s_tca9554, buf, sizeof(buf), 100);
}

static esp_err_t tca9554_read_reg(uint8_t reg, uint8_t *value_out)
{
    return i2c_master_transmit_receive(s_tca9554, &reg, 1, value_out, 1, 100);
}

static void watchdog_cb(void *arg)
{
    (void)arg;
    /* Independent of whatever the driving task believes it's doing —
     * DRAINMASTER-VALVES.md §7 Rule 3. Errors here are logged, not
     * propagated: there is no caller to return them to from a timer
     * callback, and the log line itself is the important part. */
    ESP_LOGW(TAG, "watchdog: %u ms ceiling reached, releasing relays",
             (unsigned)VALVE_DRIVE_CEILING_MS);
    esp_err_t err = tca9554_write_reg(TCA9554_REG_OUTPUT, VALVE_MASK_ALL_OFF);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "watchdog: relay release write failed: %s", esp_err_to_name(err));
    }
}

esp_err_t valve_control_driver_init(i2c_master_bus_handle_t bus)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TCA9554_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_tca9554),
                         TAG, "add TCA9554 device");

    /* Probe: a config-register read fails cleanly (I2C NACK) if nothing is
     * at 0x20, which is exactly bring-up step 1's question. */
    uint8_t probe;
    esp_err_t err = tca9554_read_reg(TCA9554_REG_CONFIG, &probe);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TCA9554 not responding at 0x%02x: %s",
                 TCA9554_I2C_ADDR, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "TCA9554 present at 0x%02x", TCA9554_I2C_ADDR);

    /* All eight relay-driver pins to output, all off, before anything else
     * touches them. */
    ESP_RETURN_ON_ERROR(tca9554_write_reg(TCA9554_REG_OUTPUT, VALVE_MASK_ALL_OFF),
                         TAG, "clear outputs");
    ESP_RETURN_ON_ERROR(tca9554_write_reg(TCA9554_REG_CONFIG, 0x00u),
                         TAG, "configure all-output");

    const gpio_config_t di_cfg = {
        .pin_bit_mask = (1ULL << VALVE_DI_GPIO_GREY) | (1ULL << VALVE_DI_GPIO_BLACK),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&di_cfg), TAG, "configure DI GPIOs");

    const esp_timer_create_args_t timer_args = {
        .callback = watchdog_cb,
        .name = "valve_watchdog",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_watchdog), TAG, "create watchdog timer");

    ESP_LOGI(TAG, "driver initialized");
    return ESP_OK;
}

esp_err_t valve_control_apply(uint8_t mask)
{
    if (!valve_mask_is_safe(mask)) {
        ESP_LOGE(TAG, "interlock: refused mask 0x%02x", mask);
        tca9554_write_reg(TCA9554_REG_OUTPUT, VALVE_MASK_ALL_OFF);
        esp_timer_stop(s_watchdog); /* idempotent if already stopped */
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = tca9554_write_reg(TCA9554_REG_OUTPUT, mask);
    if (err != ESP_OK) {
        return err;
    }

    /* Rearm cleanly on every apply rather than assuming the previous timer
     * ran to completion or was never started. */
    esp_timer_stop(s_watchdog);
    if (mask != VALVE_MASK_ALL_OFF) {
        return esp_timer_start_once(s_watchdog, (uint64_t)VALVE_DRIVE_CEILING_MS * 1000u);
    }
    return ESP_OK;
}

esp_err_t valve_control_release_all(void)
{
    return valve_control_apply(VALVE_MASK_ALL_OFF);
}

esp_err_t valve_control_read_relays(uint8_t *mask_out)
{
    return tca9554_read_reg(TCA9554_REG_INPUT, mask_out);
}

bool valve_control_read_di(valve_id_t valve)
{
    const gpio_num_t pin = (valve == VALVE_GREY) ? VALVE_DI_GPIO_GREY : VALVE_DI_GPIO_BLACK;
    return gpio_get_level(pin) != 0;
}
