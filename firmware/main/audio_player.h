#pragma once

#include "esp_err.h"
#include <stdbool.h>

/* Set to 1 in audio_player.h to re-enable classical music playback. */
#define ENABLE_AUDIO_MUSIC  0

/* Set to 1 in audio_player.h to re-enable mic record/playback. */
#define ENABLE_AUDIO_RECORD_PLAYBACK  0

#define AUDIO_SR_SAMPLE_RATE_HZ  16000

esp_err_t audio_player_init(void);
esp_err_t audio_record_playback_start(void);
esp_err_t audio_player_start(void);

esp_err_t audio_io_open_mono_16k(void);
esp_err_t audio_io_open_output(uint32_t sample_rate_hz);
esp_err_t audio_input_read(int16_t *samples, size_t byte_count);
esp_err_t audio_output_set_volume(int volume_percent);
esp_err_t audio_output_write(const int16_t *samples, size_t byte_count);

void audio_codec_pause_input(bool pause);
bool audio_codec_input_paused(void);

esp_err_t audio_play_tone(uint32_t freq_hz, uint32_t duration_ms, int volume_percent);
esp_err_t audio_play_classical_tune(volatile bool *stop_flag);

esp_err_t audio_tts_init(void);
esp_err_t audio_tts_play_joke(void);
esp_err_t audio_tts_speak(const char *text, volatile bool *stop_flag);

esp_err_t audio_sr_start(void);
void audio_sr_stop(void);
void audio_sr_pause(bool pause);
bool audio_sr_is_running(void);
