#include "audio_player.h"

#include "board_config.h"

#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "model_path.h"
#include "reminder_ui.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "audio_sr";

#define SR_CMD_OK                1

#define SR_FEED_TASK_STACK       (8 * 1024)
#define SR_DETECT_TASK_STACK     (8 * 1024)
#define SR_ACTION_TASK_STACK     (8 * 1024)
#define SR_TASK_PRIORITY         5
#define SR_ACTION_TASK_PRIORITY  4
#define SR_FEED_CORE             0
#define SR_DETECT_CORE           1
#define SR_ACTION_CORE           0
#define SR_STOP_WAIT_MS          500

typedef enum {
    SR_ACTION_ALARM_STOP = 1,
} sr_action_t;

static const esp_afe_sr_iface_t *s_afe_handle;
static esp_afe_sr_data_t *s_afe_data;
static srmodel_list_t *s_models;
static volatile int s_sr_running;
static volatile int s_sr_pause_depth;
static volatile int s_sr_paused;
static QueueHandle_t s_action_queue;

static void sr_trim_copy(const char *src, char *dst, size_t dst_size)
{
    if (dst_size == 0) {
        return;
    }

    dst[0] = '\0';
    if (src == NULL) {
        return;
    }

    while (*src != '\0' && isspace((unsigned char)*src)) {
        src++;
    }

    size_t len = strlen(src);
    while (len > 0 && isspace((unsigned char)src[len - 1])) {
        len--;
    }

    if (len >= dst_size) {
        len = dst_size - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static const char *sr_phrase_for_result(const esp_mn_results_t *result, char *buf, size_t buf_size)
{
    const char *src = NULL;

    if (result->raw_string[0] != '\0') {
        src = result->raw_string;
    } else if (result->string[0] != '\0') {
        src = result->string;
    } else if (result->num > 0) {
        src = esp_mn_commands_get_string(result->command_id[0]);
    }

    sr_trim_copy(src, buf, buf_size);
    return buf;
}

static bool sr_phrase_is_ok(const char *phrase)
{
    if (phrase == NULL) {
        return false;
    }

    while (*phrase != '\0' && isspace((unsigned char)*phrase)) {
        phrase++;
    }

    return strcasecmp(phrase, "ok") == 0;
}

static bool sr_ok_detection_accepted(const esp_mn_results_t *result, char *phrase, size_t phrase_size)
{
    if (result == NULL || result->num <= 0) {
        return false;
    }

    if (result->command_id[0] != SR_CMD_OK) {
        return false;
    }

    if (result->prob[0] < SR_OK_MIN_PROB) {
        return false;
    }

    sr_phrase_for_result(result, phrase, phrase_size);
    return sr_phrase_is_ok(phrase);
}

static void sr_post_action(sr_action_t action)
{
    if (s_action_queue == NULL) {
        return;
    }
    if (xQueueSend(s_action_queue, &action, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Action queue full, dropped action %d", (int)action);
    }
}

static void sr_action_task(void *arg)
{
    (void)arg;

    sr_action_t action;
    while (s_sr_running) {
        if (xQueueReceive(s_action_queue, &action, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }

        if (action == SR_ACTION_ALARM_STOP) {
            reminder_ui_alarm_request_stop();
        }
    }

    vTaskDelete(NULL);
}

static bool sr_io_paused(void)
{
    return s_sr_paused || audio_codec_input_paused();
}

static void sr_feed_task(void *arg)
{
    esp_afe_sr_data_t *afe_data = arg;
    const int chunk_samples = s_afe_handle->get_feed_chunksize(afe_data);
    const int channels = s_afe_handle->get_feed_channel_num(afe_data);
    int16_t *buffer = malloc(chunk_samples * channels * sizeof(int16_t));
    if (buffer == NULL) {
        ESP_LOGE(TAG, "feed buffer alloc failed");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Speech feed task running (%d samples x %d ch)", chunk_samples, channels);

    while (s_sr_running) {
        if (sr_io_paused()) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (audio_input_read(buffer, chunk_samples * channels * sizeof(int16_t)) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        s_afe_handle->feed(afe_data, buffer);
    }

    free(buffer);
    vTaskDelete(NULL);
}

static void sr_detect_task(void *arg)
{
    esp_afe_sr_data_t *afe_data = arg;

    char *mn_name = esp_srmodel_filter(s_models, ESP_MN_PREFIX, ESP_MN_ENGLISH);
    if (mn_name == NULL) {
        ESP_LOGE(TAG, "English MultiNet model not found");
        vTaskDelete(NULL);
        return;
    }

    esp_mn_iface_t *multinet = esp_mn_handle_from_name(mn_name);
    model_iface_data_t *model_data = multinet->create(mn_name, 6000);
    if (model_data == NULL) {
        ESP_LOGE(TAG, "MultiNet create failed");
        vTaskDelete(NULL);
        return;
    }

    const int mn_chunk = multinet->get_samp_chunksize(model_data);
    const int afe_chunk = s_afe_handle->get_fetch_chunksize(afe_data);
    if (mn_chunk != afe_chunk) {
        ESP_LOGE(TAG, "AFE/MultiNet chunk mismatch");
        multinet->destroy(model_data);
        vTaskDelete(NULL);
        return;
    }

    esp_mn_commands_clear();
    esp_mn_commands_add(SR_CMD_OK, (char *)"ok");
    esp_mn_error_t *cmd_err = esp_mn_commands_update();
    if (cmd_err != NULL && cmd_err->num > 0) {
        for (int i = 0; i < cmd_err->num; i++) {
            if (cmd_err->phrases[i] != NULL && cmd_err->phrases[i]->string != NULL) {
                ESP_LOGE(TAG, "Command update error: %s", cmd_err->phrases[i]->string);
            }
        }
    }
    multinet->print_active_speech_commands(model_data);
    if (multinet->set_det_threshold != NULL) {
        multinet->set_det_threshold(model_data, SR_OK_DET_THRESHOLD);
    }
    ESP_LOGI(TAG, "Listening for \"ok\" (threshold=%.2f, min prob=%.2f)",
             SR_OK_DET_THRESHOLD, SR_OK_MIN_PROB);

    while (s_sr_running) {
        if (sr_io_paused()) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        afe_fetch_result_t *res = s_afe_handle->fetch(afe_data);
        if (!res || res->ret_value == ESP_FAIL) {
            ESP_LOGW(TAG, "AFE fetch failed");
            break;
        }

        if (s_sr_paused) {
            continue;
        }

        esp_mn_state_t state = multinet->detect(model_data, res->data);
        if (state == ESP_MN_STATE_DETECTING) {
            continue;
        }

        if (state == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *result = multinet->get_results(model_data);
            char phrase[ESP_MN_MAX_PHRASE_LEN + 1];

            if (!sr_ok_detection_accepted(result, phrase, sizeof(phrase))) {
                if (result != NULL && result->num > 0) {
                    sr_phrase_for_result(result, phrase, sizeof(phrase));
                    ESP_LOGI(TAG, "Ignored detection: %s (command_id=%d, prob=%.2f)",
                             phrase,
                             result->command_id[0],
                             result->prob[0]);
                } else {
                    ESP_LOGI(TAG, "Ignored detection: no result");
                }
                multinet->clean(model_data);
                continue;
            }

            ESP_LOGI(TAG, "Heard: %s (command_id=%d, phrase_id=%d, prob=%.2f)",
                     phrase,
                     result->command_id[0],
                     result->phrase_id[0],
                     result->prob[0]);

            sr_post_action(SR_ACTION_ALARM_STOP);
            multinet->clean(model_data);
            continue;
        }
    }

    multinet->destroy(model_data);
    vTaskDelete(NULL);
}

esp_err_t audio_sr_start(void)
{
    ESP_RETURN_ON_FALSE(s_sr_running == 0, ESP_ERR_INVALID_STATE, TAG, "already running");

    s_models = esp_srmodel_init("model");
    ESP_RETURN_ON_FALSE(s_models, ESP_FAIL, TAG, "Speech models not found (flash model partition?)");

    ESP_RETURN_ON_ERROR(audio_io_open_mono_16k(), TAG, "codec open for SR failed");

    afe_config_t *afe_config = afe_config_init("M", s_models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    ESP_RETURN_ON_FALSE(afe_config, ESP_FAIL, TAG, "AFE config failed");
    afe_config->wakenet_init = false;

    s_afe_handle = esp_afe_handle_from_config(afe_config);
    s_afe_data = s_afe_handle->create_from_config(afe_config);
    afe_config_free(afe_config);
    ESP_RETURN_ON_FALSE(s_afe_data, ESP_FAIL, TAG, "AFE create failed");

    s_action_queue = xQueueCreate(4, sizeof(sr_action_t));
    ESP_RETURN_ON_FALSE(s_action_queue, ESP_ERR_NO_MEM, TAG, "action queue failed");

    s_sr_running = 1;
    s_sr_pause_depth = 0;
    s_sr_paused = 0;

    BaseType_t ok = xTaskCreatePinnedToCore(
        sr_action_task, "sr_action", SR_ACTION_TASK_STACK, NULL, SR_ACTION_TASK_PRIORITY, NULL, SR_ACTION_CORE);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "action task failed");

    ok = xTaskCreatePinnedToCore(
        sr_feed_task, "sr_feed", SR_FEED_TASK_STACK, s_afe_data, SR_TASK_PRIORITY, NULL, SR_FEED_CORE);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "feed task failed");

    ok = xTaskCreatePinnedToCore(
        sr_detect_task, "sr_detect", SR_DETECT_TASK_STACK, s_afe_data, SR_TASK_PRIORITY, NULL, SR_DETECT_CORE);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "detect task failed");

    ESP_LOGI(TAG, "Speech recognition started");
    return ESP_OK;
}

void audio_sr_stop(void)
{
    if (!s_sr_running) {
        return;
    }

    s_sr_running = 0;
    vTaskDelay(pdMS_TO_TICKS(SR_STOP_WAIT_MS));

    if (s_afe_data != NULL && s_afe_handle != NULL) {
        s_afe_handle->destroy(s_afe_data);
        s_afe_data = NULL;
    }

    if (s_action_queue != NULL) {
        vQueueDelete(s_action_queue);
        s_action_queue = NULL;
    }

    s_sr_pause_depth = 0;
    s_sr_paused = 0;
    ESP_LOGI(TAG, "Speech recognition stopped");
}

void audio_sr_pause(bool pause)
{
    if (!s_sr_running) {
        return;
    }

    if (pause) {
        s_sr_pause_depth++;
        s_sr_paused = 1;
        return;
    }

    if (s_sr_pause_depth > 0) {
        s_sr_pause_depth--;
    }
    if (s_sr_pause_depth == 0) {
        s_sr_paused = 0;
    }
}

bool audio_sr_is_running(void)
{
    return s_sr_running != 0;
}
