# Reminder — ESP-IDF firmware

Firmware for the **Reminder** app on the Freenove FNK0104 (ESP32-S3) board:

- **Display:** ILI9341, 320×240 landscape (SPI)
- **Touch:** FT6336U (I2C)
- **UI:** LVGL 9 welcome screen

## Requirements

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) v5.4 or newer
- Freenove FNK0104 (ESP32-S3, 2.8" ILI9341 + FT6336U)

## Build and flash

```bash
cd firmware
idf.py set-target esp32s3
idf.py build flash monitor -b 115200
```

On boot you should see:

```
I (xxx) Reminder: Welcome To Reminder...
```

The LCD shows **Reminder** with a **Welcome** subtitle.

## Pin map (FNK0104)

| Signal | GPIO | Notes |
|--------|------|-------|
| LCD CS | 10 | SPI |
| LCD MOSI | 11 | SPI |
| LCD SCK | 12 | SPI |
| LCD MISO | 13 | SPI |
| LCD RS (`lcd_rs`) | 46 | Register Select (D/C) |
| LCD backlight | 45 | LEDC PWM |
| Touch SDA | 16 | I2C |
| Touch SCL | 15 | I2C |
| Touch RST | 18 | |
| Touch INT | 17 | |

LCD reset is not broken out to a GPIO (power-on reset).

## Backlight brightness

Default is **100%**. Change in [`main/board_config.h`](main/board_config.h):

```c
#define ILI9341_BACKLIGHT_BRIGHTNESS  80
```

Or override before including `board_config.h` in a source file.

Runtime adjustment: `ili9341_display_set_brightness(percent)`.

## Project layout

```
main/
  board_config.h      Pin definitions
  ili9341_display.c   ILI9341 + PWM backlight
  ft6336u_touch.c     FT6336U touch
  main.c              app_main + LVGL welcome UI
```
