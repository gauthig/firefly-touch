#include "ws_io_expander.h"

#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "ws_io_expander";

#define WS_IOEXP_ADDR    0x24

#define WS_IOEXP_REG_MODE    0x02
#define WS_IOEXP_REG_OUTPUT  0x03
#define WS_IOEXP_REG_INPUT   0x04
#define WS_IOEXP_REG_PWM     0x05

/* Demo caps PWM at 97 "to prevent the screen from completely turning off";
 * we keep 0 meaning off, so cap the top instead. */
#define WS_IOEXP_PWM_MAX 250

static i2c_master_dev_handle_t s_dev;
static uint8_t s_io_shadow;

static esp_err_t wr_reg(uint8_t reg, uint8_t value)
{
    const uint8_t buf[2] = { reg, value };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 100);
}

esp_err_t ws_io_expander_init(i2c_master_bus_handle_t bus)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = WS_IOEXP_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_dev), TAG, "add dev");

    /* All EXIO to output; start every line low (matches board_lcd7.c's
     * CH422G bring-up -- USB_SEL low so native USB stays alive until
     * board_twai_init(), backlight off until the panel is up). */
    ESP_RETURN_ON_ERROR(wr_reg(WS_IOEXP_REG_MODE, 0xFF), TAG, "mode=output");
    s_io_shadow = 0x00;
    ESP_RETURN_ON_ERROR(wr_reg(WS_IOEXP_REG_OUTPUT, s_io_shadow), TAG, "outputs low");
    ESP_LOGI(TAG, "IO_EXTENSION @0x24 initialized (outputs, all low)");
    return ESP_OK;
}

esp_err_t ws_io_expander_write_io(uint8_t value)
{
    s_io_shadow = value;
    return wr_reg(WS_IOEXP_REG_OUTPUT, s_io_shadow);
}

esp_err_t ws_io_expander_set_pin(uint8_t pin, bool level)
{
    if (pin > 7) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t next = level ? (uint8_t)(s_io_shadow | (1u << pin))
                               : (uint8_t)(s_io_shadow & ~(1u << pin));
    return ws_io_expander_write_io(next);
}

esp_err_t ws_io_expander_set_backlight_pct(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    const uint8_t duty = (uint8_t)((percent * WS_IOEXP_PWM_MAX) / 100);
    return wr_reg(WS_IOEXP_REG_PWM, duty);
}
