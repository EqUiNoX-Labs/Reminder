#pragma once

#include "esp_err.h"
#include "lvgl.h"
#include "reminder_storage.h"

esp_err_t reminder_ui_init(lv_display_t *display);

void reminder_ui_show_init(void);
void reminder_ui_set_init_progress(int percent, const char *status);

void reminder_ui_show_set_time(void);
esp_err_t reminder_ui_wait_time_set(void);

void reminder_ui_show_clock(void);
void reminder_ui_show_settings(void);

bool reminder_ui_alarm_is_active(void);
void reminder_ui_alarm_request_stop(void);

const reminder_settings_t *reminder_ui_get_settings(void);
