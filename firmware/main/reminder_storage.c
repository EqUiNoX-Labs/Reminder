#include "reminder_storage.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "reminder_nvs";
static const char *NS = "reminder";

void reminder_entry_format_default_text(uint8_t hour24, uint8_t minute, char *buf, size_t buf_size)
{
    bool is_pm = hour24 >= 12;
    int hour12 = hour24 % 12;
    if (hour12 == 0) {
        hour12 = 12;
    }

    snprintf(buf, buf_size, "This is your %d:%02d %s reminder.",
             hour12, minute, is_pm ? "pm" : "am");
}

void reminder_entry_set_default_text(reminder_entry_t *entry)
{
    if (entry == NULL) {
        return;
    }

    reminder_entry_format_default_text(entry->hour24, entry->minute, entry->text, sizeof(entry->text));
}

esp_err_t reminder_storage_load(reminder_settings_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t u8;
    int64_t i64;

    if (nvs_get_u8(nvs, "time_h", &u8) == ESP_OK) {
        out->hour24 = u8;
    }
    if (nvs_get_u8(nvs, "time_m", &u8) == ESP_OK) {
        out->minute = u8;
    }
    if (nvs_get_u8(nvs, "time_s", &u8) == ESP_OK) {
        out->second = u8;
    }
    if (nvs_get_i64(nvs, "epoch_ms", &i64) == ESP_OK) {
        out->epoch_ms = i64;
    }

    uint8_t loaded_count = 0;
    bool slot_enabled[REMINDER_COUNT];
    reminder_entry_t slots[REMINDER_COUNT];
    memset(slots, 0, sizeof(slots));
    memset(slot_enabled, 0, sizeof(slot_enabled));

    for (int i = 0; i < REMINDER_COUNT; i++) {
        char key[8];

        snprintf(key, sizeof(key), "r%d_en", i);
        if (nvs_get_u8(nvs, key, &u8) == ESP_OK) {
            slot_enabled[i] = (u8 != 0);
        }
        snprintf(key, sizeof(key), "r%d_h", i);
        if (nvs_get_u8(nvs, key, &u8) == ESP_OK) {
            slots[i].hour24 = u8;
        }
        snprintf(key, sizeof(key), "r%d_m", i);
        if (nvs_get_u8(nvs, key, &u8) == ESP_OK) {
            slots[i].minute = u8;
        }
        snprintf(key, sizeof(key), "r%d_t", i);
        size_t text_len = sizeof(slots[i].text);
        if (nvs_get_str(nvs, key, slots[i].text, &text_len) != ESP_OK) {
            slots[i].text[0] = '\0';
        }
    }

    if (nvs_get_u8(nvs, "rem_count", &u8) == ESP_OK) {
        out->reminder_count = u8;
        if (out->reminder_count > REMINDER_COUNT) {
            out->reminder_count = REMINDER_COUNT;
        }

        for (int i = 0; i < out->reminder_count; i++) {
            out->reminders[i] = slots[i];
            if (out->reminders[i].text[0] == '\0') {
                reminder_entry_set_default_text(&out->reminders[i]);
            }
        }
    } else {
        for (int i = 0; i < REMINDER_COUNT; i++) {
            if (slot_enabled[i] && loaded_count < REMINDER_COUNT) {
                out->reminders[loaded_count] = slots[i];
                if (out->reminders[loaded_count].text[0] == '\0') {
                    reminder_entry_set_default_text(&out->reminders[loaded_count]);
                }
                loaded_count++;
            }
        }
        out->reminder_count = loaded_count;
    }

    nvs_close(nvs);
    ESP_LOGI(TAG, "Settings loaded (%u reminders)", out->reminder_count);
    return ESP_OK;
}

esp_err_t reminder_storage_save(const reminder_settings_t *settings)
{
    if (settings == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_u8(nvs, "time_h", settings->hour24));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_u8(nvs, "time_m", settings->minute));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_u8(nvs, "time_s", settings->second));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_i64(nvs, "epoch_ms", settings->epoch_ms));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_u8(nvs, "time_ok", 1));
    ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_u8(nvs, "rem_count", settings->reminder_count));

    for (int i = 0; i < REMINDER_COUNT; i++) {
        char key[8];
        if (i < settings->reminder_count) {
            snprintf(key, sizeof(key), "r%d_h", i);
            ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_u8(nvs, key, settings->reminders[i].hour24));
            snprintf(key, sizeof(key), "r%d_m", i);
            ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_u8(nvs, key, settings->reminders[i].minute));
            snprintf(key, sizeof(key), "r%d_t", i);
            ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_str(nvs, key, settings->reminders[i].text));
        }
        snprintf(key, sizeof(key), "r%d_en", i);
        ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_u8(nvs, key, (i < settings->reminder_count) ? 1 : 0));
    }

    err = nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG, "Settings saved (%u reminders)", settings->reminder_count);
    return err;
}

bool reminder_storage_has_saved_time(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NS, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }

    uint8_t ok = 0;
    const esp_err_t err = nvs_get_u8(nvs, "time_ok", &ok);
    nvs_close(nvs);
    return (err == ESP_OK && ok != 0);
}
