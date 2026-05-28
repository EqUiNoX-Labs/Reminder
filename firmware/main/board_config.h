#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"

/* FNK0104 board wiring for ILI9341 (landscape 320x240) and FT6336U touch. */

#define ILI9341_H_RES            320
#define ILI9341_V_RES            240
#define ILI9341_SPI_HOST         SPI2_HOST
#define ILI9341_PIN_CS           GPIO_NUM_10
#define ILI9341_PIN_MOSI         GPIO_NUM_11
#define ILI9341_PIN_SCK          GPIO_NUM_12
#define ILI9341_PIN_MISO         GPIO_NUM_13
#define ILI9341_PIN_RS           GPIO_NUM_46  /* schematic: lcd_rs -> ILI9341 RS (dc_gpio_num) */
#define ILI9341_PIN_BL           GPIO_NUM_45
#define ILI9341_SPI_HZ           (40 * 1000 * 1000)

#ifndef ILI9341_BACKLIGHT_BRIGHTNESS
#define ILI9341_BACKLIGHT_BRIGHTNESS  100
#endif

#define FT6336U_PIN_SDA          GPIO_NUM_16
#define FT6336U_PIN_SCL          GPIO_NUM_15
#define FT6336U_PIN_RST          GPIO_NUM_18
#define FT6336U_PIN_INT          GPIO_NUM_17
#define FT6336U_I2C_PORT         I2C_NUM_0

/* Touch mapping for 240x320 native panel used in 320x240 landscape.
 * Raw FT6336 coords are portrait (X:0..239, Y:0..319). All transforms run in
 * ft6336u_touch.c — keep driver mirror/swap flags disabled to avoid double apply.
 * With SWAP_XY: MIRROR_Y flips screen X, MIRROR_X flips screen Y. */
#define FT6336U_RAW_X_MAX        (ILI9341_V_RES - 1)
#define FT6336U_RAW_Y_MAX        (ILI9341_H_RES - 1)
#define FT6336U_TOUCH_SWAP_XY    1
#define FT6336U_TOUCH_MIRROR_X   0
#define FT6336U_TOUCH_MIRROR_Y   1
#define FT6336U_TOUCH_OFFSET_X   0
#define FT6336U_TOUCH_OFFSET_Y   0

#define BOARD_I2C_PORT           FT6336U_I2C_PORT
#define BOARD_I2C_PIN_SDA        FT6336U_PIN_SDA
#define BOARD_I2C_PIN_SCL        FT6336U_PIN_SCL

/* ES8311 codec + NS4150 PA (FNK0104). */
#define I2S_PIN_MCK              GPIO_NUM_4
#define I2S_PIN_BCK              GPIO_NUM_5
#define I2S_PIN_DIN              GPIO_NUM_6
#define I2S_PIN_WS               GPIO_NUM_7
#define I2S_PIN_DOUT             GPIO_NUM_8
#define AUDIO_PA_PIN             GPIO_NUM_1
#define AUDIO_SAMPLE_RATE        44100
#define AUDIO_OUTPUT_VOLUME      70
#define AUDIO_TTS_VOLUME         100   /* codec DAC level for speech (0–100) */
#define AUDIO_TTS_TARGET_PEAK    28000 /* ~85% full scale */
#define AUDIO_INPUT_GAIN         42.0f   /* ES8311 max analog PGA (0–42 dB) */
#define AUDIO_RECORD_SECONDS     6
#define AUDIO_RECORD_TARGET_PEAK 32767   /* normalize to 100% full scale */
#define AUDIO_PLAYBACK_SPEED     1.0f   /* 1.0 = normal speed */

#define AUDIO_TTS_DELAY_MS       0
#define AUDIO_TTS_PRERENDER_AT_BOOT  1     /* cache joke in PSRAM after boot for instant play */
#define AUDIO_TTS_RENDER_MAX_BYTES     (256 * 1024)
#define AUDIO_TTS_PLAY_CHUNK_SAMPLES   512
#define AUDIO_TTS_TASK_PRIORITY        8     /* above SR (5) while synthesizing */
#define AUDIO_TTS_TASK_CORE            0     /* keep core 1 free for speech detect */
#define AUDIO_WAKE_TONE_HZ             880
#define AUDIO_WAKE_TONE_MS             150
#define AUDIO_WAKE_TONE_VOLUME         80

#define ALARM_PAUSE_MS                 5000
#define ALARM_MAX_CYCLES               10
#define ALARM_TUNE_VOLUME              70

#define BOARD_UART_BAUD_RATE     115200
