#include "audio_player.h"

#include "board_config.h"

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "picotts.h"

#include <string.h>

static const char *TAG = "audio_tts";

#define TTS_SYNTH_TIMEOUT_MS     120000
#define TTS_PRERENDER_STACK      4096
#define TTS_PRERENDER_TASK_PRIO  4

#ifndef AUDIO_TTS_RENDER_MAX_BYTES
#define AUDIO_TTS_RENDER_MAX_BYTES  (256 * 1024)
#endif

#ifndef AUDIO_TTS_PLAY_CHUNK_SAMPLES
#define AUDIO_TTS_PLAY_CHUNK_SAMPLES   512
#endif

#ifndef AUDIO_TTS_TASK_PRIORITY
#define AUDIO_TTS_TASK_PRIORITY    8
#endif

#ifndef AUDIO_TTS_TASK_CORE
#define AUDIO_TTS_TASK_CORE        0
#endif

#ifndef AUDIO_TTS_PRERENDER_AT_BOOT
#define AUDIO_TTS_PRERENDER_AT_BOOT  1
#endif

static const char s_funny_quote[] =
    "Why do programmers prefer dark mode? Because light attracts bugs.";

typedef struct {
    int16_t *buf;
    size_t samples;
    size_t capacity_samples;
    bool overflow;
} tts_render_ctx_t;

static SemaphoreHandle_t s_tts_mutex;
static SemaphoreHandle_t s_tts_idle_sem;
static SemaphoreHandle_t s_joke_ready_sem;
static tts_render_ctx_t *s_render_ctx;

static int16_t *s_joke_buf;
static size_t s_joke_samples;
static bool s_joke_ready;
static volatile bool s_joke_prerendering;

static void tts_idle_cb(void)
{
    if (s_tts_idle_sem) {
        xSemaphoreGive(s_tts_idle_sem);
    }
}

static void tts_render_cb(int16_t *samples, unsigned count)
{
    tts_render_ctx_t *ctx = s_render_ctx;
    if (ctx == NULL || ctx->overflow || ctx->buf == NULL || samples == NULL || count == 0) {
        return;
    }

    if (ctx->samples + count > ctx->capacity_samples) {
        ctx->overflow = true;
        ESP_LOGE(TAG, "Render buffer full (%u samples max)", (unsigned)ctx->capacity_samples);
        return;
    }

    memcpy(&ctx->buf[ctx->samples], samples, count * sizeof(int16_t));
    ctx->samples += count;
}

static void tts_normalize_buffer(int16_t *samples, size_t count)
{
    int32_t peak = 0;

    for (size_t i = 0; i < count; i++) {
        int32_t level = samples[i];
        if (level < 0) {
            level = -level;
        }
        if (level > peak) {
            peak = level;
        }
    }

    if (peak <= 256) {
        return;
    }

    const int32_t gain = (AUDIO_TTS_TARGET_PEAK * 256) / peak;
    for (size_t i = 0; i < count; i++) {
        int32_t sample = ((int32_t)samples[i] * gain) >> 8;
        if (sample > 32767) {
            sample = 32767;
        } else if (sample < -32768) {
            sample = -32768;
        }
        samples[i] = (int16_t)sample;
    }
}

static esp_err_t tts_synthesize_text(const char *text, tts_render_ctx_t *ctx)
{
    if (text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t buf_bytes = AUDIO_TTS_RENDER_MAX_BYTES;
    ctx->buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ctx->buf == NULL) {
        ctx->buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    ESP_RETURN_ON_FALSE(ctx->buf, ESP_ERR_NO_MEM, TAG, "Render alloc failed");

    ctx->samples = 0;
    ctx->capacity_samples = buf_bytes / sizeof(int16_t);
    ctx->overflow = false;

    s_tts_idle_sem = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_tts_idle_sem, ESP_ERR_NO_MEM, TAG, "idle semaphore create failed");

    s_render_ctx = ctx;
    if (!picotts_init(AUDIO_TTS_TASK_PRIORITY, tts_render_cb, AUDIO_TTS_TASK_CORE)) {
        s_render_ctx = NULL;
        vSemaphoreDelete(s_tts_idle_sem);
        s_tts_idle_sem = NULL;
        heap_caps_free(ctx->buf);
        ctx->buf = NULL;
        ESP_RETURN_ON_ERROR(ESP_FAIL, TAG, "PicoTTS init failed");
    }

    picotts_set_idle_notify(tts_idle_cb);
    picotts_add(text, strlen(text) + 1);

    if (xSemaphoreTake(s_tts_idle_sem, pdMS_TO_TICKS(TTS_SYNTH_TIMEOUT_MS)) != pdTRUE) {
        picotts_shutdown();
        s_render_ctx = NULL;
        vSemaphoreDelete(s_tts_idle_sem);
        s_tts_idle_sem = NULL;
        heap_caps_free(ctx->buf);
        ctx->buf = NULL;
        ESP_RETURN_ON_ERROR(ESP_ERR_TIMEOUT, TAG, "Render timed out");
    }

    picotts_shutdown();
    s_render_ctx = NULL;
    vSemaphoreDelete(s_tts_idle_sem);
    s_tts_idle_sem = NULL;

    ESP_RETURN_ON_FALSE(!ctx->overflow && ctx->samples > 0, ESP_FAIL, TAG, "Render failed");

    tts_normalize_buffer(ctx->buf, ctx->samples);
    return ESP_OK;
}

static esp_err_t tts_synthesize(tts_render_ctx_t *ctx)
{
    return tts_synthesize_text(s_funny_quote, ctx);
}

static esp_err_t tts_cache_joke(void)
{
    tts_render_ctx_t ctx = {0};
    esp_err_t err = tts_synthesize(&ctx);
    if (err != ESP_OK) {
        return err;
    }

    if (s_joke_buf != NULL) {
        heap_caps_free(s_joke_buf);
    }

    s_joke_buf = ctx.buf;
    s_joke_samples = ctx.samples;
    s_joke_ready = true;
    ESP_LOGI(TAG, "Joke cached: %u samples (%.1f s)",
             (unsigned)s_joke_samples,
             (float)s_joke_samples / PICOTTS_SAMPLE_FREQ_HZ);
    return ESP_OK;
}

static void tts_prerender_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Background pre-render started");
    if (tts_cache_joke() == ESP_OK && s_joke_ready_sem != NULL) {
        xSemaphoreGive(s_joke_ready_sem);
    }
    s_joke_prerendering = false;
    vTaskDelete(NULL);
}

static esp_err_t tts_ensure_joke_cached(void)
{
    if (s_joke_ready) {
        return ESP_OK;
    }

    audio_sr_pause(true);

    if (s_joke_prerendering && s_joke_ready_sem != NULL) {
        ESP_LOGI(TAG, "Waiting for background pre-render");
        if (xSemaphoreTake(s_joke_ready_sem, pdMS_TO_TICKS(TTS_SYNTH_TIMEOUT_MS)) == pdTRUE &&
            s_joke_ready) {
            audio_sr_pause(false);
            return ESP_OK;
        }
    }

    ESP_LOGI(TAG, "Rendering speech");
    esp_err_t err = tts_cache_joke();
    audio_sr_pause(false);

    if (err == ESP_OK && s_joke_ready_sem != NULL) {
        xSemaphoreGive(s_joke_ready_sem);
    }
    return err;
}

static esp_err_t tts_play_buffer(const int16_t *buf, size_t samples, volatile bool *stop_flag)
{
    ESP_RETURN_ON_FALSE(buf && samples > 0, ESP_ERR_INVALID_STATE, TAG, "nothing to play");

    esp_err_t err = audio_io_open_mono_16k();
    ESP_RETURN_ON_ERROR(err, TAG, "codec open failed");

    if (audio_output_set_volume(AUDIO_TTS_VOLUME) != ESP_OK) {
        ESP_LOGW(TAG, "TTS volume set failed, continuing at default");
    }

    size_t pos = 0;
    while (pos < samples) {
        if (stop_flag != NULL && *stop_flag) {
            return ESP_OK;
        }

        size_t chunk = samples - pos;
        if (chunk > AUDIO_TTS_PLAY_CHUNK_SAMPLES) {
            chunk = AUDIO_TTS_PLAY_CHUNK_SAMPLES;
        }

        if (audio_output_write(&buf[pos], chunk * sizeof(int16_t)) != ESP_OK) {
            ESP_LOGW(TAG, "speaker write failed at sample %u", (unsigned)pos);
            return ESP_FAIL;
        }
        pos += chunk;
    }

    return ESP_OK;
}

static esp_err_t tts_play_cached(void)
{
    ESP_RETURN_ON_FALSE(s_joke_buf && s_joke_samples > 0, ESP_ERR_INVALID_STATE, TAG, "nothing to play");

    ESP_LOGI(TAG, "Playing joke");
    return tts_play_buffer(s_joke_buf, s_joke_samples, NULL);
}

esp_err_t audio_tts_init(void)
{
    if (s_tts_mutex != NULL) {
        return ESP_OK;
    }

    s_tts_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_tts_mutex, ESP_ERR_NO_MEM, TAG, "TTS mutex create failed");

    s_joke_ready_sem = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_joke_ready_sem, ESP_ERR_NO_MEM, TAG, "ready semaphore create failed");

#if AUDIO_TTS_PRERENDER_AT_BOOT
    s_joke_prerendering = true;
    BaseType_t ok = xTaskCreatePinnedToCore(
        tts_prerender_task,
        "tts_prerender",
        TTS_PRERENDER_STACK,
        NULL,
        TTS_PRERENDER_TASK_PRIO,
        NULL,
        AUDIO_TTS_TASK_CORE);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "pre-render task failed");
    ESP_LOGI(TAG, "TTS ready (pre-rendering in background)");
#else
    ESP_LOGI(TAG, "TTS ready (render on demand)");
#endif

    return ESP_OK;
}

esp_err_t audio_tts_play_joke(void)
{
    ESP_RETURN_ON_FALSE(s_tts_mutex, ESP_ERR_INVALID_STATE, TAG, "TTS not initialized");

    if (xSemaphoreTake(s_tts_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Joke already playing");
        return ESP_ERR_INVALID_STATE;
    }

    audio_codec_pause_input(true);
    vTaskDelay(pdMS_TO_TICKS(50));

    esp_err_t err = tts_ensure_joke_cached();
    if (err == ESP_OK) {
        err = tts_play_cached();
    }

    audio_codec_pause_input(false);
    xSemaphoreGive(s_tts_mutex);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Joke finished");
    }
    return err;
}

esp_err_t audio_tts_speak(const char *text, volatile bool *stop_flag)
{
    ESP_RETURN_ON_FALSE(s_tts_mutex, ESP_ERR_INVALID_STATE, TAG, "TTS not initialized");
    ESP_RETURN_ON_FALSE(text && text[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "empty text");

    if (xSemaphoreTake(s_tts_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Speech already playing");
        return ESP_ERR_INVALID_STATE;
    }

    audio_sr_pause(true);
    audio_codec_pause_input(true);
    vTaskDelay(pdMS_TO_TICKS(50));

    tts_render_ctx_t ctx = {0};
    esp_err_t err = tts_synthesize_text(text, &ctx);
    if (err == ESP_OK) {
        err = tts_play_buffer(ctx.buf, ctx.samples, stop_flag);
    }

    if (ctx.buf != NULL) {
        heap_caps_free(ctx.buf);
    }

    audio_codec_pause_input(false);
    audio_sr_pause(false);
    xSemaphoreGive(s_tts_mutex);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Speech finished");
    }
    return err;
}
