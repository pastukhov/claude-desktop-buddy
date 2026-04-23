#pragma once

#define USER_SETUP_LOADED

#define USER_SETUP_ID 251
#define USER_SETUP_INFO "ESP32-S3-BOX-3B"

#define USE_HSPI_PORT
#define M5STACK
#define ST7789_DRIVER
#define TFT_WIDTH  320
#define TFT_HEIGHT 240

#define TFT_MISO 0
#define TFT_MOSI 6
#define TFT_SCLK 7
#define TFT_CS   5
#define TFT_DC   4
#define TFT_RST  48

#define TFT_RGB_ORDER TFT_BGR
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

#define SPI_FREQUENCY       27000000
#define SPI_READ_FREQUENCY  20000000
#define SPI_TOUCH_FREQUENCY 2500000
#define SUPPORT_TRANSACTIONS
