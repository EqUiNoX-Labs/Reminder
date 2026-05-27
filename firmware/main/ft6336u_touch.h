#pragma once

#include "esp_err.h"
#include "esp_lcd_touch.h"

esp_err_t ft6336u_touch_init(esp_lcd_touch_handle_t *touch_out);
