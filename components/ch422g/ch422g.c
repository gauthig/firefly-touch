#include "ch422g.h"

#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "ch422g";

#define CH422G_ADDR_WR_SET  0x24
#define CH422G_ADDR_WR_OC   0x23
#define CH422G_ADDR_WR_IO   0x38

/* WR_SET bit0: IO_OE — enable push-pull outputs on IO0..IO7 */
#define CH422G_WR_SET_IO_OE 0x01

static i2c_master_dev_handle_t s_dev_set;
static i2c_master_dev_handle_t s_dev_io;
static uint8_t s_io_shadow = 0x00;

static esp_err_t add_dev(i2c_master_bus_handle_t bus, uint16_t addr,
                         i2c_master_dev_handle_t *out)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    return i2c_master_bus_add_device(bus, &cfg, out);
}

static esp_err_t wr_byte(i2c_master_dev_handle_t dev, uint8_t value)
{
    return i2c_master_transmit(dev, &value, 1, 100);
}

esp_err_t ch422g_init(i2c_master_bus_handle_t bus)
{
    ESP_RETURN_ON_ERROR(add_dev(bus, CH422G_ADDR_WR_SET, &s_dev_set), TAG, "add WR_SET");
    ESP_RETURN_ON_ERROR(add_dev(bus, CH422G_ADDR_WR_IO, &s_dev_io), TAG, "add WR_IO");

    ESP_RETURN_ON_ERROR(wr_byte(s_dev_set, CH422G_WR_SET_IO_OE), TAG, "enable outputs");
    s_io_shadow = 0x00;
    ESP_RETURN_ON_ERROR(wr_byte(s_dev_io, s_io_shadow), TAG, "clear outputs");
    ESP_LOGI(TAG, "CH422G initialized (outputs enabled, all low)");
    return ESP_OK;
}

esp_err_t ch422g_write_io(uint8_t value)
{
    s_io_shadow = value;
    return wr_byte(s_dev_io, s_io_shadow);
}

esp_err_t ch422g_set_pin(uint8_t pin, bool level)
{
    if (pin > 7) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t next = level ? (uint8_t)(s_io_shadow | (1u << pin))
                         : (uint8_t)(s_io_shadow & ~(1u << pin));
    return ch422g_write_io(next);
}
