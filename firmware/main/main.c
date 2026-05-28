#include "board_config.h"
#include "board_i2c.h"
#include "audio_player.h"
#include "ft6336u_touch.h"
#include "ili9341_display.h"
#include "reminder_ui.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "Reminder";

#define STARTUP_TASK_STACK       (12 * 1024)
#define STARTUP_TASK_PRIORITY    5
#define STARTUP_TASK_CORE        0

static void reminder_log_memory(void)
{
    const size_t internal_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    ESP_LOGI(TAG, "Memory internal: %u KB total, %u KB free",
             (unsigned)(internal_total / 1024), (unsigned)(internal_free / 1024));

    if (psram_total > 0) {
        ESP_LOGI(TAG, "Memory PSRAM:    %u KB total, %u KB free",
                 (unsigned)(psram_total / 1024), (unsigned)(psram_free / 1024));
    }
}

static void reminder_ui_progress(int percent, const char *status)
{
    lvgl_port_lock(0);
    reminder_ui_set_init_progress(percent, status);
    lvgl_port_unlock();
}

static void reminder_startup_task(void *arg)
{
    (void)arg;

    reminder_ui_progress(20, "Audio codec...");
    ESP_ERROR_CHECK(audio_player_init());

    reminder_ui_progress(50, "Speech synthesis...");
    ESP_ERROR_CHECK(audio_tts_init());

    reminder_ui_progress(100, "Ready");
    vTaskDelay(pdMS_TO_TICKS(500));

    lvgl_port_lock(0);
    reminder_ui_show_set_time();
    lvgl_port_unlock();

    ESP_LOGI(TAG, "Waiting for time to be set on screen");
    ESP_ERROR_CHECK(reminder_ui_wait_time_set());

    lvgl_port_lock(0);
    reminder_ui_show_clock();
    lvgl_port_unlock();

    reminder_log_memory();
    ESP_LOGI(TAG, "Clock screen running");

    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Welcome To Reminder...");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(board_i2c_init());

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
            .mirror_x = true,
            .mirror_y = true,
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

    ESP_ERROR_CHECK(reminder_ui_init(display));

    lvgl_port_lock(0);
    reminder_ui_show_init();
    reminder_ui_set_init_progress(0, "Starting...");
    lvgl_port_unlock();

    BaseType_t ok = xTaskCreatePinnedToCore(
        reminder_startup_task,
        "reminder_start",
        STARTUP_TASK_STACK,
        NULL,
        STARTUP_TASK_PRIORITY,
        NULL,
        STARTUP_TASK_CORE);
    ESP_ERROR_CHECK(ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
