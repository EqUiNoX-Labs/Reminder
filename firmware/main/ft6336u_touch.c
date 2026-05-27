#include "ft6336u_touch.h"

#include "board_config.h"

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_ft6x36.h"
#include "esp_log.h"

static const char *TAG = "ft6336u";

static i2c_master_bus_handle_t s_i2c_bus;

esp_err_t ft6336u_touch_init(esp_lcd_touch_handle_t *touch_out)
{
    ESP_RETURN_ON_FALSE(touch_out, ESP_ERR_INVALID_ARG, TAG, "null output pointer");

    ESP_LOGI(TAG, "Initialize I2C (SDA=%d SCL=%d)", FT6336U_PIN_SDA, FT6336U_PIN_SCL);
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = FT6336U_I2C_PORT,
        .sda_io_num = FT6336U_PIN_SDA,
        .scl_io_num = FT6336U_PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c_bus), TAG, "I2C bus init failed");

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_FT6x36_CONFIG();
    tp_io_config.scl_speed_hz = 400000;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c_bus, &tp_io_config, &tp_io_handle),
                        TAG, "touch panel IO init failed");

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = ILI9341_H_RES,
        .y_max = ILI9341_V_RES,
        .rst_gpio_num = FT6336U_PIN_RST,
        .int_gpio_num = FT6336U_PIN_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 1,
            .mirror_x = 0,
            .mirror_y = 1,
        },
    };

    ESP_LOGI(TAG, "Install FT6336U driver");
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_ft6x36(tp_io_handle, &tp_cfg, touch_out),
                        TAG, "FT6336U init failed");

    ESP_LOGI(TAG, "FT6336U ready");
    return ESP_OK;
}
