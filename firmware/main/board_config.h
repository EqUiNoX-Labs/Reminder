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

#define BOARD_UART_BAUD_RATE     115200
