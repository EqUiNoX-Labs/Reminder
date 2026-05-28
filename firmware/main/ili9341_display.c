#include "ili9341_display.h"

#include "board_config.h"

#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"

static const char *TAG = "ili9341";

#define LEDC_TIMER              LEDC_TIMER_0
#define LEDC_MODE               LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL            LEDC_CHANNEL_0
#define LEDC_DUTY_RES           LEDC_TIMER_10_BIT
#define LEDC_FREQUENCY_HZ       5000

static bool s_backlight_ready;
static uint32_t s_backlight_max_duty;

static esp_err_t ili9341_backlight_init(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_MODE,
        .duty_resolution = LEDC_DUTY_RES,
        .timer_num = LEDC_TIMER,
        .freq_hz = LEDC_FREQUENCY_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), TAG, "LEDC timer config failed");

    ledc_channel_config_t channel_cfg = {
        .gpio_num = ILI9341_PIN_BL,
        .speed_mode = LEDC_MODE,
        .channel = LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel_cfg), TAG, "LEDC channel config failed");

    s_backlight_max_duty = (1U << LEDC_DUTY_RES) - 1;
    s_backlight_ready = true;
    return ESP_OK;
}

esp_err_t ili9341_display_fill(esp_lcd_panel_handle_t panel, uint16_t rgb565)
{
    uint16_t line[ILI9341_H_RES];
    for (int x = 0; x < ILI9341_H_RES; x++) {
        line[x] = rgb565;
    }

    for (int y = 0; y < ILI9341_V_RES; y++) {
        ESP_RETURN_ON_ERROR(
            esp_lcd_panel_draw_bitmap(panel, 0, y, ILI9341_H_RES, y + 1, line),
            TAG, "panel fill failed");
    }

    return ESP_OK;
}

esp_err_t ili9341_display_set_brightness(uint8_t percent)
{
    if (!s_backlight_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (percent > 100) {
        percent = 100;
    }
    uint32_t duty = (s_backlight_max_duty * percent) / 100;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty), TAG, "set duty failed");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_MODE, LEDC_CHANNEL), TAG, "update duty failed");
    return ESP_OK;
}

esp_err_t ili9341_display_init(esp_lcd_panel_io_handle_t *io_out,
                               esp_lcd_panel_handle_t *panel_out)
{
    ESP_RETURN_ON_FALSE(io_out && panel_out, ESP_ERR_INVALID_ARG, TAG, "null output pointer");

    ESP_LOGI(TAG, "Initialize SPI bus (host %d)", ILI9341_SPI_HOST);
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = ILI9341_PIN_MOSI,
        .miso_io_num = ILI9341_PIN_MISO,
        .sclk_io_num = ILI9341_PIN_SCK,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = ILI9341_H_RES * ILI9341_V_RES * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(ILI9341_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO),
                        TAG, "SPI bus init failed");

    ESP_LOGI(TAG, "Install panel IO (CS=%d RS=%d)", ILI9341_PIN_CS, ILI9341_PIN_RS);
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = ILI9341_PIN_CS,
        .dc_gpio_num = ILI9341_PIN_RS,
        .spi_mode = 0,
        .pclk_hz = ILI9341_SPI_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(ILI9341_SPI_HOST, &io_config, &io_handle),
                        TAG, "panel IO init failed");

    ESP_LOGI(TAG, "Install ILI9341 driver");
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ili9341(io_handle, &panel_config, &panel_handle),
                        TAG, "ILI9341 panel init failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel_handle), TAG, "panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(panel_handle, true), TAG, "invert failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(panel_handle, true), TAG, "swap_xy failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(panel_handle, true, true), TAG, "mirror failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel_handle, true), TAG, "display on failed");

    /* Wipe GRAM before backlight — clears ghost from prior firmware/demo. */
    ESP_RETURN_ON_ERROR(ili9341_display_fill(panel_handle, 0x0000), TAG, "panel clear failed");

    ESP_RETURN_ON_ERROR(ili9341_backlight_init(), TAG, "backlight init failed");
    ESP_RETURN_ON_ERROR(ili9341_display_set_brightness(ILI9341_BACKLIGHT_BRIGHTNESS),
                        TAG, "backlight set failed");
    ESP_LOGI(TAG, "Backlight PWM at %d%%", ILI9341_BACKLIGHT_BRIGHTNESS);

    *io_out = io_handle;
    *panel_out = panel_handle;
    ESP_LOGI(TAG, "ILI9341 ready (%dx%d landscape)", ILI9341_H_RES, ILI9341_V_RES);
    return ESP_OK;
}
