#include "board_config.h"
#include "ft6336u_touch.h"
#include "ili9341_display.h"

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "nvs_flash.h"

static const char *TAG = "Reminder";

static void reminder_create_welcome_screen(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1a1a2e), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Reminder");
    lv_obj_set_style_text_color(title, lv_color_hex(0xeeeeee), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -12);

    lv_obj_t *subtitle = lv_label_create(scr);
    lv_label_set_text(subtitle, "Welcome");
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_16, 0);
    lv_obj_align(subtitle, LV_ALIGN_CENTER, 0, 24);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Welcome To Reminder...");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    esp_lcd_panel_io_handle_t lcd_io = NULL;
    esp_lcd_panel_handle_t lcd_panel = NULL;
    ESP_ERROR_CHECK(ili9341_display_init(&lcd_io, &lcd_panel));

    esp_lcd_touch_handle_t touch = NULL;
    ESP_ERROR_CHECK(ft6336u_touch_init(&touch));

    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = lcd_io,
        .panel_handle = lcd_panel,
        .buffer_size = ILI9341_H_RES * 40,
        .double_buffer = true,
        .hres = ILI9341_H_RES,
        .vres = ILI9341_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = true,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
            .swap_bytes = true,
        },
    };
    lv_display_t *display = lvgl_port_add_disp(&disp_cfg);
    ESP_ERROR_CHECK(display ? ESP_OK : ESP_FAIL);

    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = display,
        .handle = touch,
    };
    ESP_ERROR_CHECK(lvgl_port_add_touch(&touch_cfg) ? ESP_OK : ESP_FAIL);

    lvgl_port_lock(0);
    reminder_create_welcome_screen();
    lvgl_port_unlock();

    ESP_LOGI(TAG, "Reminder welcome screen running");
}
