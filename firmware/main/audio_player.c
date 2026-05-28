#include "audio_player.h"

#include "board_config.h"
#include "board_i2c.h"

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

#include <math.h>
#include <stdlib.h>

static const char *TAG = "audio";

#define AUDIO_I2S_PORT           I2S_NUM_0
#define AUDIO_IO_CHUNK_BYTES     1024
#define AUDIO_TASK_STACK         8192
#define AUDIO_TASK_PRIORITY      5
#define AUDIO_TASK_CORE          1

#define AUDIO_MONO_FRAME_BYTES   sizeof(int16_t)
#define AUDIO_RECORD_BYTES       (AUDIO_SAMPLE_RATE * AUDIO_MONO_FRAME_BYTES * AUDIO_RECORD_SECONDS)

static i2s_chan_handle_t s_tx_handle;
static i2s_chan_handle_t s_rx_handle;
static esp_codec_dev_handle_t s_codec;
static bool s_codec_open;
#if ENABLE_AUDIO_RECORD_PLAYBACK
static int16_t *s_record_buf;
static TaskHandle_t s_audio_task;
#elif ENABLE_AUDIO_MUSIC
static TaskHandle_t s_audio_task;
#endif

#include "classical_tune.h"

#define AUDIO_CHUNK_FRAMES       256
#define REVERB_SIZE              4096
#define HARMONIC_COUNT           4

static const float s_harmonic_gain[HARMONIC_COUNT] = {1.0f, 0.45f, 0.22f, 0.11f};

typedef struct {
    float phase[HARMONIC_COUNT];
    float freq_hz;
    float target_amp;
    float env;
    float attack_step;
    float release_step;
    bool active;
    bool releasing;
    float vibrato_phase;
} synth_voice_t;

typedef struct {
    size_t note_index;
    uint32_t note_samples_left;
    synth_voice_t voice;
} tune_sequencer_t;

static float s_reverb_buf[REVERB_SIZE];
static int s_reverb_pos;
static uint32_t s_synth_sample_rate = AUDIO_SAMPLE_RATE;

static float midi_to_hz(uint8_t midi)
{
    return 440.0f * powf(2.0f, ((float)midi - 69.0f) / 12.0f);
}

static float soft_clip(float sample)
{
    if (sample > 1.0f) {
        return 1.0f;
    }
    if (sample < -1.0f) {
        return -1.0f;
    }
    return sample * (1.5f - 0.5f * sample * sample);
}

static float apply_reverb(float sample)
{
    float feedback = s_reverb_buf[s_reverb_pos];
    s_reverb_buf[s_reverb_pos] = sample + feedback * 0.42f;
    s_reverb_pos = (s_reverb_pos + 1) % REVERB_SIZE;
    return sample * 0.72f + feedback * 0.38f;
}

static void voice_start(synth_voice_t *voice, float freq_hz, float velocity)
{
    voice->freq_hz = freq_hz;
    voice->target_amp = velocity * 0.007f;
    voice->env = 0.0f;
    voice->active = true;
    voice->releasing = false;
    voice->vibrato_phase = 0.0f;
    voice->attack_step = voice->target_amp / (0.012f * (float)s_synth_sample_rate);
    voice->release_step = voice->target_amp / (0.18f * (float)s_synth_sample_rate);
}

static void voice_release(synth_voice_t *voice)
{
    voice->releasing = true;
}

static float voice_render(synth_voice_t *voice)
{
    if (!voice->active) {
        return 0.0f;
    }

    if (voice->releasing) {
        voice->env -= voice->release_step;
        if (voice->env <= 0.0f) {
            voice->env = 0.0f;
            voice->active = false;
            return 0.0f;
        }
    } else if (voice->env < voice->target_amp) {
        voice->env += voice->attack_step;
        if (voice->env > voice->target_amp) {
            voice->env = voice->target_amp;
        }
    }

    voice->vibrato_phase += 2.0f * (float)M_PI * 5.2f / (float)s_synth_sample_rate;
    if (voice->vibrato_phase > 2.0f * (float)M_PI) {
        voice->vibrato_phase -= 2.0f * (float)M_PI;
    }

    float vibrato = 1.0f + 0.004f * sinf(voice->vibrato_phase);
    float sample = 0.0f;

    for (int h = 0; h < HARMONIC_COUNT; h++) {
        float harmonic_freq = voice->freq_hz * (float)(h + 1) * vibrato;
        voice->phase[h] += 2.0f * (float)M_PI * harmonic_freq / (float)s_synth_sample_rate;
        if (voice->phase[h] > 2.0f * (float)M_PI) {
            voice->phase[h] -= 2.0f * (float)M_PI;
        }

        float harmonic_env = voice->env * powf(0.82f, (float)h);
        sample += sinf(voice->phase[h]) * s_harmonic_gain[h] * harmonic_env;
    }

    return soft_clip(sample * 0.55f);
}

static void sequencer_begin_note(tune_sequencer_t *seq, const tune_note_t *note)
{
    if (note->midi_note == 0) {
        voice_release(&seq->voice);
    } else {
        voice_start(&seq->voice, midi_to_hz(note->midi_note), (float)note->velocity);
    }
    seq->note_samples_left = ((uint32_t)note->duration_ms * s_synth_sample_rate) / 1000U;
}

#if ENABLE_AUDIO_MUSIC

static void sequencer_advance(tune_sequencer_t *seq)
{
    const tune_note_t *note = &classical_tune[seq->note_index];
    sequencer_begin_note(seq, note);
    seq->note_index++;
    if (seq->note_index >= classical_tune_count) {
        seq->note_index = 0;
    }
}

static void render_chunk(int16_t *stereo_out, tune_sequencer_t *seq)
{
    for (int i = 0; i < AUDIO_CHUNK_FRAMES; i++) {
        if (seq->note_samples_left == 0) {
            if (seq->voice.active && !seq->voice.releasing) {
                voice_release(&seq->voice);
            }
            sequencer_advance(seq);
        } else {
            seq->note_samples_left--;
        }

        float mono = voice_render(&seq->voice);
        mono = apply_reverb(mono);

        float right = mono * (1.0f + 0.0008f * sinf((float)i * 0.11f));
        float left = mono;

        int32_t l = (int32_t)(left * 32767.0f);
        int32_t r = (int32_t)(right * 32767.0f);
        if (l > 32767) {
            l = 32767;
        } else if (l < -32768) {
            l = -32768;
        }
        if (r > 32767) {
            r = 32767;
        } else if (r < -32768) {
            r = -32768;
        }

        stereo_out[i * 2] = (int16_t)l;
        stereo_out[i * 2 + 1] = (int16_t)r;
    }
}

#endif /* ENABLE_AUDIO_MUSIC */

static bool sequencer_advance_one_shot(tune_sequencer_t *seq)
{
    if (seq->note_index >= classical_tune_count) {
        voice_release(&seq->voice);
        seq->note_samples_left = 0;
        return false;
    }

    sequencer_begin_note(seq, &classical_tune[seq->note_index]);
    seq->note_index++;
    return true;
}

static bool render_chunk_one_shot_mono(int16_t *mono_out, tune_sequencer_t *seq)
{
    bool finished = false;

    for (int i = 0; i < AUDIO_CHUNK_FRAMES; i++) {
        if (seq->note_samples_left == 0) {
            if (seq->voice.active && !seq->voice.releasing) {
                voice_release(&seq->voice);
            }
            if (!sequencer_advance_one_shot(seq)) {
                finished = true;
            }
        } else {
            seq->note_samples_left--;
        }

        float mono = voice_render(&seq->voice);
        mono = apply_reverb(mono);

        int32_t sample = (int32_t)(mono * 32767.0f);
        if (sample > 32767) {
            sample = 32767;
        } else if (sample < -32768) {
            sample = -32768;
        }

        mono_out[i] = (int16_t)sample;
    }

    return finished && !seq->voice.active;
}

esp_err_t audio_play_classical_tune(volatile bool *stop_flag)
{
    ESP_RETURN_ON_FALSE(s_codec, ESP_ERR_INVALID_STATE, TAG, "codec not init");

    const uint32_t prev_rate = s_synth_sample_rate;
    s_synth_sample_rate = AUDIO_SR_SAMPLE_RATE_HZ;

    if (!s_codec_open) {
        esp_err_t open_err = audio_io_open_mono_16k();
        if (open_err != ESP_OK) {
            s_synth_sample_rate = prev_rate;
            return open_err;
        }
    }

    if (audio_output_set_volume(ALARM_TUNE_VOLUME) != ESP_OK) {
        ESP_LOGW(TAG, "tune volume set failed, continuing");
    }

    memset(s_reverb_buf, 0, sizeof(s_reverb_buf));
    s_reverb_pos = 0;

    tune_sequencer_t seq = {0};
    int16_t buffer[AUDIO_CHUNK_FRAMES];
    esp_err_t err = ESP_OK;

    if (!sequencer_advance_one_shot(&seq)) {
        s_synth_sample_rate = prev_rate;
        return ESP_FAIL;
    }

    while (true) {
        if (stop_flag != NULL && *stop_flag) {
            break;
        }

        const bool finished = render_chunk_one_shot_mono(buffer, &seq);
        if (audio_output_write(buffer, sizeof(buffer)) != ESP_OK) {
            ESP_LOGW(TAG, "tune write failed");
            err = ESP_FAIL;
            break;
        }

        if (finished) {
            break;
        }
    }

    s_synth_sample_rate = prev_rate;
    return err;
}

#if ENABLE_AUDIO_MUSIC

static esp_err_t audio_open_stereo_output(uint32_t sample_rate_hz, int volume_percent)
{
    ESP_RETURN_ON_FALSE(s_codec, ESP_ERR_INVALID_STATE, TAG, "codec not created");

    if (s_codec_open) {
        esp_codec_dev_close(s_codec);
        s_codec_open = false;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 2,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1),
        .sample_rate = sample_rate_hz,
    };
    if (esp_codec_dev_open(s_codec, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "stereo open failed (%lu Hz)", (unsigned long)sample_rate_hz);
        return ESP_FAIL;
    }
    if (esp_codec_dev_set_out_vol(s_codec, volume_percent) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "stereo volume set failed");
    }
    s_codec_open = true;
    return ESP_OK;
}

static void audio_player_task(void *arg)
{
    tune_sequencer_t seq = {0};
    int16_t buffer[AUDIO_CHUNK_FRAMES * 2];

    ESP_LOGI(TAG, "Playing Morning Mood (soft synth, core %d)", AUDIO_TASK_CORE);
    if (audio_open_stereo_output(AUDIO_SAMPLE_RATE, AUDIO_OUTPUT_VOLUME) != ESP_OK) {
        ESP_LOGE(TAG, "Music output open failed");
        vTaskDelete(NULL);
        return;
    }

    memset(s_reverb_buf, 0, sizeof(s_reverb_buf));
    s_reverb_pos = 0;
    sequencer_advance(&seq);

    while (true) {
        render_chunk(buffer, &seq);
        if (esp_codec_dev_write(s_codec, buffer, sizeof(buffer)) != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "codec write failed");
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

esp_err_t audio_player_start(void)
{
    ESP_RETURN_ON_FALSE(s_codec, ESP_ERR_INVALID_STATE, TAG, "audio not initialized");
    ESP_RETURN_ON_FALSE(s_audio_task == NULL, ESP_ERR_INVALID_STATE, TAG, "already running");

    memset(s_reverb_buf, 0, sizeof(s_reverb_buf));
    s_reverb_pos = 0;

    BaseType_t ok = xTaskCreatePinnedToCore(
        audio_player_task,
        "audio_player",
        AUDIO_TASK_STACK,
        NULL,
        AUDIO_TASK_PRIORITY,
        &s_audio_task,
        AUDIO_TASK_CORE);

    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "audio task create failed");
    return ESP_OK;
}

#else /* ENABLE_AUDIO_MUSIC */

esp_err_t audio_player_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* ENABLE_AUDIO_MUSIC */

static esp_err_t audio_i2s_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_handle, &s_rx_handle), TAG, "I2S channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_PIN_MCK,
            .bclk = I2S_PIN_BCK,
            .ws = I2S_PIN_WS,
            .dout = I2S_PIN_DOUT,
            .din = I2S_PIN_DIN,
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_handle, &std_cfg), TAG, "I2S TX init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_handle, &std_cfg), TAG, "I2S RX init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_handle), TAG, "I2S TX enable failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_handle), TAG, "I2S RX enable failed");
    return ESP_OK;
}

static esp_err_t audio_codec_init(void)
{
    i2c_master_bus_handle_t i2c_bus = board_i2c_get_bus();
    ESP_RETURN_ON_FALSE(i2c_bus, ESP_ERR_INVALID_STATE, TAG, "I2C bus not ready");

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BOARD_I2C_PORT,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(ctrl_if, ESP_ERR_NO_MEM, TAG, "codec ctrl if failed");

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = AUDIO_I2S_PORT,
        .rx_handle = s_rx_handle,
        .tx_handle = s_tx_handle,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(data_if, ESP_ERR_NO_MEM, TAG, "codec data if failed");

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(gpio_if, ESP_ERR_NO_MEM, TAG, "codec gpio if failed");

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = AUDIO_PA_PIN,
        .use_mclk = true,
        .pa_reverted = true,
        .no_dac_ref = true,
        .hw_gain = {
            .pa_voltage = 5.0f,
            .codec_dac_voltage = 3.3f,
        },
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es8311_cfg);
    ESP_RETURN_ON_FALSE(codec_if, ESP_ERR_NO_MEM, TAG, "ES8311 codec if failed");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_codec = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_codec, ESP_ERR_NO_MEM, TAG, "codec dev failed");

    ESP_LOGI(TAG, "ES8311 codec ready");
    return ESP_OK;
}

esp_err_t audio_output_open(uint32_t sample_rate_hz)
{
    ESP_RETURN_ON_FALSE(s_codec, ESP_ERR_INVALID_STATE, TAG, "codec not created");

    if (s_codec_open) {
        esp_codec_dev_close(s_codec);
        s_codec_open = false;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
        .sample_rate = sample_rate_hz,
    };
    if (esp_codec_dev_open(s_codec, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec open failed (%lu Hz)", (unsigned long)sample_rate_hz);
        return ESP_FAIL;
    }
    if (esp_codec_dev_set_out_vol(s_codec, AUDIO_OUTPUT_VOLUME) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "set output volume failed");
        return ESP_FAIL;
    }
    if (esp_codec_dev_set_in_gain(s_codec, AUDIO_INPUT_GAIN) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "set input gain failed");
        return ESP_FAIL;
    }
#if ENABLE_AUDIO_RECORD_PLAYBACK
    if (esp_codec_dev_set_in_gain(s_codec, AUDIO_INPUT_GAIN) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "set input gain failed");
        return ESP_FAIL;
    }
#endif
    s_codec_open = true;
    ESP_LOGI(TAG, "Audio output open (%lu Hz mono)", (unsigned long)sample_rate_hz);
    return ESP_OK;
}

esp_err_t audio_output_set_volume(int volume_percent)
{
    ESP_RETURN_ON_FALSE(s_codec && s_codec_open, ESP_ERR_INVALID_STATE, TAG, "output not open");
    if (volume_percent < 0) {
        volume_percent = 0;
    } else if (volume_percent > 100) {
        volume_percent = 100;
    }
    if (esp_codec_dev_set_out_vol(s_codec, volume_percent) != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t audio_output_write(const int16_t *samples, size_t byte_count)
{
    ESP_RETURN_ON_FALSE(s_codec && s_codec_open, ESP_ERR_INVALID_STATE, TAG, "output not open");
    ESP_RETURN_ON_FALSE(samples && byte_count > 0, ESP_ERR_INVALID_ARG, TAG, "invalid buffer");

    if (esp_codec_dev_write(s_codec, (void *)samples, byte_count) != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool s_input_paused;
static int s_input_pause_depth;

esp_err_t audio_io_open_mono_16k(void)
{
    return audio_output_open(AUDIO_SR_SAMPLE_RATE_HZ);
}

esp_err_t audio_io_open_output(uint32_t sample_rate_hz)
{
    return audio_output_open(sample_rate_hz);
}

esp_err_t audio_input_read(int16_t *samples, size_t byte_count)
{
    ESP_RETURN_ON_FALSE(s_codec && s_codec_open, ESP_ERR_INVALID_STATE, TAG, "codec not open");
    ESP_RETURN_ON_FALSE(samples && byte_count > 0, ESP_ERR_INVALID_ARG, TAG, "invalid buffer");

    if (esp_codec_dev_read(s_codec, samples, byte_count) != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

void audio_codec_pause_input(bool pause)
{
    if (pause) {
        s_input_pause_depth++;
        s_input_paused = true;
        return;
    }

    if (s_input_pause_depth > 0) {
        s_input_pause_depth--;
    }
    if (s_input_pause_depth == 0) {
        s_input_paused = false;
        if (s_codec != NULL) {
            audio_io_open_mono_16k();
        }
    }
}

bool audio_codec_input_paused(void)
{
    return s_input_paused;
}

esp_err_t audio_play_tone(uint32_t freq_hz, uint32_t duration_ms, int volume_percent)
{
    if (freq_hz == 0 || duration_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    audio_codec_pause_input(true);
    vTaskDelay(pdMS_TO_TICKS(20));

    esp_err_t err = audio_io_open_mono_16k();
    if (err != ESP_OK) {
        audio_codec_pause_input(false);
        return err;
    }
    if (audio_output_set_volume(volume_percent) != ESP_OK) {
        ESP_LOGW(TAG, "tone volume set failed, continuing");
    }

    const uint32_t sample_rate = AUDIO_SR_SAMPLE_RATE_HZ;
    const size_t total_samples = (size_t)sample_rate * duration_ms / 1000U;
    const size_t ramp_samples = sample_rate / 200U; /* 5 ms fade in/out */
    int16_t chunk[256];

    for (size_t pos = 0; pos < total_samples; ) {
        size_t count = total_samples - pos;
        if (count > sizeof(chunk) / sizeof(chunk[0])) {
            count = sizeof(chunk) / sizeof(chunk[0]);
        }

        for (size_t i = 0; i < count; i++) {
            const size_t idx = pos + i;
            const float phase = (2.0f * (float)M_PI * (float)freq_hz * (float)idx) / (float)sample_rate;
            float amp = 0.35f;
            if (idx < ramp_samples) {
                amp *= (float)idx / (float)ramp_samples;
            } else if (idx + ramp_samples > total_samples) {
                amp *= (float)(total_samples - idx) / (float)ramp_samples;
            }
            const int32_t sample = (int32_t)(sinf(phase) * amp * 32767.0f);
            chunk[i] = (int16_t)sample;
        }

        if (audio_output_write(chunk, count * sizeof(int16_t)) != ESP_OK) {
            audio_codec_pause_input(false);
            return ESP_FAIL;
        }
        pos += count;
    }

    audio_codec_pause_input(false);
    return ESP_OK;
}

#if ENABLE_AUDIO_RECORD_PLAYBACK
static esp_err_t audio_read_chunk(size_t byte_offset, size_t byte_count)
{
    uint8_t *dst = (uint8_t *)s_record_buf + byte_offset;
    while (byte_count > 0) {
        size_t chunk = byte_count > AUDIO_IO_CHUNK_BYTES ? AUDIO_IO_CHUNK_BYTES : byte_count;
        if (esp_codec_dev_read(s_codec, dst, chunk) != ESP_CODEC_DEV_OK) {
            return ESP_FAIL;
        }
        dst += chunk;
        byte_count -= chunk;
    }
    return ESP_OK;
}

static int16_t audio_lerp_mono(size_t frame_idx, float frac)
{
    int32_t a = s_record_buf[frame_idx];
    int32_t b = s_record_buf[frame_idx + 1];
    int32_t sample = (int32_t)(a + (b - a) * frac);
    if (sample > 32767) {
        return 32767;
    }
    if (sample < -32768) {
        return -32768;
    }
    return (int16_t)sample;
}

static esp_err_t audio_playback_mono(void)
{
    const size_t mono_frames = AUDIO_RECORD_BYTES / AUDIO_MONO_FRAME_BYTES;
    const size_t out_frames = (size_t)((float)mono_frames / AUDIO_PLAYBACK_SPEED);
    int16_t chunk[AUDIO_IO_CHUNK_BYTES / sizeof(int16_t)];
    size_t chunk_len = 0;

    for (size_t out = 0; out < out_frames; out++) {
        const float pos = (float)out * AUDIO_PLAYBACK_SPEED;
        const size_t idx = (size_t)pos;
        const float frac = pos - (float)idx;

        if (idx + 1 >= mono_frames) {
            break;
        }

        chunk[chunk_len++] = audio_lerp_mono(idx, frac);

        if (chunk_len >= sizeof(chunk) / sizeof(chunk[0])) {
            if (esp_codec_dev_write(s_codec, chunk, chunk_len * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
                return ESP_FAIL;
            }
            chunk_len = 0;
        }
    }

    if (chunk_len > 0) {
        if (esp_codec_dev_write(s_codec, chunk, chunk_len * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

static void audio_post_process_recording(void)
{
    const size_t samples = AUDIO_RECORD_BYTES / AUDIO_MONO_FRAME_BYTES;
    int32_t peak = 0;

    for (size_t i = 0; i < samples; i++) {
        int32_t level = abs(s_record_buf[i]);
        if (level > peak) {
            peak = level;
        }
    }

    if (peak < 512) {
        ESP_LOGW(TAG, "Recording very quiet (peak=%ld) — try speaking louder", (long)peak);
        return;
    }

    for (size_t i = 0; i < samples; i++) {
        int32_t sample = ((int32_t)s_record_buf[i] * AUDIO_RECORD_TARGET_PEAK) / peak;
        if (sample > 32767) {
            sample = 32767;
        } else if (sample < -32768) {
            sample = -32768;
        }
        s_record_buf[i] = (int16_t)sample;
    }

    ESP_LOGI(TAG, "Recording boosted (raw peak=%ld)", (long)peak);
}

static void record_playback_task(void *arg)
{
    (void)arg;

    while (true) {
        if (audio_read_chunk(0, AUDIO_RECORD_BYTES) != ESP_OK) {
            ESP_LOGE(TAG, "Recording failed");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        audio_post_process_recording();

        if (audio_playback_mono() != ESP_OK) {
            ESP_LOGE(TAG, "Playback failed");
        }
    }
}

#else /* ENABLE_AUDIO_RECORD_PLAYBACK */

esp_err_t audio_record_playback_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* ENABLE_AUDIO_RECORD_PLAYBACK */

esp_err_t audio_player_init(void)
{
    ESP_RETURN_ON_ERROR(audio_i2s_init(), TAG, "I2S init failed");
    ESP_RETURN_ON_ERROR(audio_codec_init(), TAG, "codec init failed");

#if ENABLE_AUDIO_RECORD_PLAYBACK
    s_record_buf = heap_caps_malloc(AUDIO_RECORD_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_record_buf == NULL) {
        s_record_buf = heap_caps_malloc(AUDIO_RECORD_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    ESP_RETURN_ON_FALSE(s_record_buf, ESP_ERR_NO_MEM, TAG, "record buffer alloc failed");
    ESP_LOGI(TAG, "Record buffer: %d bytes (%d s mono)", AUDIO_RECORD_BYTES, AUDIO_RECORD_SECONDS);
#endif

    return ESP_OK;
}

#if ENABLE_AUDIO_RECORD_PLAYBACK
esp_err_t audio_record_playback_start(void)
{
    ESP_RETURN_ON_FALSE(s_codec && s_record_buf, ESP_ERR_INVALID_STATE, TAG, "audio not initialized");
    ESP_RETURN_ON_FALSE(s_audio_task == NULL, ESP_ERR_INVALID_STATE, TAG, "audio task already running");

    BaseType_t ok = xTaskCreatePinnedToCore(
        record_playback_task,
        "audio_rec",
        AUDIO_TASK_STACK,
        NULL,
        AUDIO_TASK_PRIORITY,
        &s_audio_task,
        AUDIO_TASK_CORE);

    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "audio task create failed");
    return ESP_OK;
}
#endif /* ENABLE_AUDIO_RECORD_PLAYBACK */
