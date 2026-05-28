#include "ft6336u_touch.h"

#include "board_config.h"
#include "board_i2c.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_ft6x36.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ft6336u";

#define FT6336U_I2C_ADDR         0x38
#define FT6336U_RESET_LOW_MS     10
#define FT6336U_RESET_HIGH_MS    500

static void ft6336u_hardware_reset(void)
{
    const gpio_config_t int_cfg = {
        .pin_bit_mask = BIT64(FT6336U_PIN_INT),
        .mode = GPIO_MODE_INPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&int_cfg));

    const gpio_config_t rst_cfg = {
        .pin_bit_mask = BIT64(FT6336U_PIN_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&rst_cfg));

    ESP_LOGI(TAG, "Reset FT6336U (RST=%d)", FT6336U_PIN_RST);
    ESP_ERROR_CHECK(gpio_set_level(FT6336U_PIN_RST, 0));
    vTaskDelay(pdMS_TO_TICKS(FT6336U_RESET_LOW_MS));
    ESP_ERROR_CHECK(gpio_set_level(FT6336U_PIN_RST, 1));
    vTaskDelay(pdMS_TO_TICKS(FT6336U_RESET_HIGH_MS));
}

static void ft6336u_log_i2c_probe(i2c_master_bus_handle_t bus)
{
    esp_err_t err = i2c_master_probe(bus, FT6336U_I2C_ADDR, 100);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "I2C probe OK at 0x%02X", FT6336U_I2C_ADDR);
        return;
    }

    ESP_LOGW(TAG, "I2C probe failed at 0x%02X (%s)", FT6336U_I2C_ADDR, esp_err_to_name(err));
    err = i2c_master_probe(bus, 0x48, 100);
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "Touch controller responded at alternate address 0x48");
    }
}

static void ft6336u_map_to_screen(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y,
                                  uint16_t *strength, uint8_t *point_num, uint8_t max_point_num)
{
    (void)tp;
    (void)strength;
    (void)max_point_num;

    for (int i = 0; i < *point_num; i++) {
        int32_t rx = x[i];
        int32_t ry = y[i];

        if (rx > FT6336U_RAW_X_MAX) {
            rx = (rx * FT6336U_RAW_X_MAX) / 4095;
        }
        if (ry > FT6336U_RAW_Y_MAX) {
            ry = (ry * FT6336U_RAW_Y_MAX) / 4095;
        }

#if FT6336U_TOUCH_MIRROR_X
        rx = FT6336U_RAW_X_MAX - rx;
#endif
#if FT6336U_TOUCH_MIRROR_Y
        ry = FT6336U_RAW_Y_MAX - ry;
#endif

        int32_t sx;
        int32_t sy;
#if FT6336U_TOUCH_SWAP_XY
        sx = ry;
        sy = rx;
#else
        sx = rx;
        sy = ry;
#endif

        sx += FT6336U_TOUCH_OFFSET_X;
        sy += FT6336U_TOUCH_OFFSET_Y;

        if (sx < 0) {
            sx = 0;
        } else if (sx >= ILI9341_H_RES) {
            sx = ILI9341_H_RES - 1;
        }

        if (sy < 0) {
            sy = 0;
        } else if (sy >= ILI9341_V_RES) {
            sy = ILI9341_V_RES - 1;
        }

        x[i] = (uint16_t)sx;
        y[i] = (uint16_t)sy;
    }
}

esp_err_t ft6336u_touch_init(esp_lcd_touch_handle_t *touch_out)
{
    ESP_RETURN_ON_FALSE(touch_out, ESP_ERR_INVALID_ARG, TAG, "null output pointer");

    ESP_LOGI(TAG, "Initialize touch I2C (shared bus, SDA=%d SCL=%d)", FT6336U_PIN_SDA, FT6336U_PIN_SCL);
    i2c_master_bus_handle_t i2c_bus = board_i2c_get_bus();
    ESP_RETURN_ON_FALSE(i2c_bus, ESP_ERR_INVALID_STATE, TAG, "I2C bus not ready");

    ft6336u_hardware_reset();
    ft6336u_log_i2c_probe(i2c_bus);

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_FT6x36_CONFIG();
    tp_io_config.scl_speed_hz = 400000;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io_config, &tp_io_handle),
                        TAG, "touch panel IO init failed");

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = FT6336U_RAW_X_MAX,
        .y_max = FT6336U_RAW_Y_MAX,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = FT6336U_PIN_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
        .process_coordinates = ft6336u_map_to_screen,
    };

    ESP_LOGI(TAG, "Install FT6336U driver (raw %dx%d swap=%d mirror_x=%d mirror_y=%d off=%d,%d)",
             FT6336U_RAW_X_MAX + 1, FT6336U_RAW_Y_MAX + 1,
             FT6336U_TOUCH_SWAP_XY, FT6336U_TOUCH_MIRROR_X, FT6336U_TOUCH_MIRROR_Y,
             FT6336U_TOUCH_OFFSET_X, FT6336U_TOUCH_OFFSET_Y);
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_ft6x36(tp_io_handle, &tp_cfg, touch_out),
                        TAG, "FT6336U init failed");

    ESP_LOGI(TAG, "FT6336U ready");
    return ESP_OK;
}
