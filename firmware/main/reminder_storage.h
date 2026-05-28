#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

#define REMINDER_COUNT 5
#define REMINDER_TEXT_LEN 64

typedef struct {
    uint8_t hour24;
    uint8_t minute;
    char text[REMINDER_TEXT_LEN];
} reminder_entry_t;

typedef struct {
    uint8_t hour24;
    uint8_t minute;
    uint8_t second;
    int64_t epoch_ms;
    uint8_t reminder_count;
    reminder_entry_t reminders[REMINDER_COUNT];
} reminder_settings_t;

esp_err_t reminder_storage_load(reminder_settings_t *out);
esp_err_t reminder_storage_save(const reminder_settings_t *settings);
bool reminder_storage_has_saved_time(void);

void reminder_entry_format_default_text(uint8_t hour24, uint8_t minute, char *buf, size_t buf_size);
void reminder_entry_set_default_text(reminder_entry_t *entry);
