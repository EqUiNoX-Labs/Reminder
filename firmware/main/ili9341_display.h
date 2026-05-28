#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

esp_err_t ili9341_display_init(esp_lcd_panel_io_handle_t *io_out,
                               esp_lcd_panel_handle_t *panel_out);

esp_err_t ili9341_display_fill(esp_lcd_panel_handle_t panel, uint16_t rgb565);

esp_err_t ili9341_display_set_brightness(uint8_t percent);
