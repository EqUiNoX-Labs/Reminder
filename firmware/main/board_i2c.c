#include "board_i2c.h"

#include "board_config.h"

#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "board_i2c";

static i2c_master_bus_handle_t s_i2c_bus;

esp_err_t board_i2c_init(void)
{
    if (s_i2c_bus) {
        return ESP_OK;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_I2C_PIN_SDA,
        .scl_io_num = BOARD_I2C_PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    ESP_LOGI(TAG, "Init shared I2C (SDA=%d SCL=%d)", BOARD_I2C_PIN_SDA, BOARD_I2C_PIN_SCL);
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c_bus), TAG, "I2C bus init failed");
    return ESP_OK;
}

i2c_master_bus_handle_t board_i2c_get_bus(void)
{
    return s_i2c_bus;
}
