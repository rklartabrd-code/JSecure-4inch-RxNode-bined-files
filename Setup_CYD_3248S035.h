#pragma once

/*
  TFT_eSPI setup for Sunton ESP32-3248S035 family
  3.5 inch 320x480 ST7796 SPI display

  Install this file into:
    Arduino/libraries/TFT_eSPI/User_Setups/

  Then add this line to TFT_eSPI/User_Setup_Select.h:
    #include <User_Setups/Setup_CYD_3248S035.h>

  and comment out any other selected setup.
*/

#define USER_SETUP_INFO "ESP32-3248S035 ST7796"

#define ST7796_DRIVER
#define TFT_WIDTH  320
#define TFT_HEIGHT 480

// TFT is wired to ESP32 HSPI on this board.
#define USE_HSPI_PORT
#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST  -1

#define TFT_BL   27
#define TFT_BACKLIGHT_ON HIGH

// Start conservatively for bring-up. Once proven, 65 MHz is commonly used.
#define SPI_FREQUENCY       65000000
#define SPI_READ_FREQUENCY  20000000

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define SMOOTH_FONT
