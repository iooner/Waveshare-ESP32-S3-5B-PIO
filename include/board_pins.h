// Brochage Waveshare ESP32-S3-LCD-5B (sans tactile)
// Sources : wiki Waveshare + config ESP32_Display_Panel (BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_5_B)
#pragma once

// --- LCD RGB565 1024x600 ---
#define LCD_WIDTH            1024
#define LCD_HEIGHT           600
// Timings resserrés pour ~37 Hz. Le plafond est la PSRAM : elle se lit à ~59 Mo/s et une image
// pèse 1,23 Mo. A 32 MHz (160/5, ~43,6 Hz) l'affichage en prend ~90 % et il ne reste que
// ~6 Mo/s pour dessiner ; stabilité de l'image à ce palier non validée visuellement.
// Valeurs d'origine (ESP32_Display_Panel), ~24 Hz : PCLK 21 MHz, HBP 160, HFP 160.
#define LCD_PCLK_HZ          (160 * 1000 * 1000 / 6)  // 26,67 MHz, diviseur entier du PLL 160 MHz
#define LCD_HPW              24
#define LCD_HBP              40
#define LCD_HFP              40
#define LCD_VPW              2
#define LCD_VBP              23
#define LCD_VFP              12
#define LCD_PCLK_ACTIVE_NEG  1

#define LCD_PIN_VSYNC  3
#define LCD_PIN_HSYNC  46
#define LCD_PIN_DE     5
#define LCD_PIN_PCLK   7

#define LCD_PIN_B3  14
#define LCD_PIN_B4  38
#define LCD_PIN_B5  18
#define LCD_PIN_B6  17
#define LCD_PIN_B7  10
#define LCD_PIN_G2  39
#define LCD_PIN_G3  0
#define LCD_PIN_G4  45
#define LCD_PIN_G5  48
#define LCD_PIN_G6  47
#define LCD_PIN_G7  21
#define LCD_PIN_R3  1
#define LCD_PIN_R4  2
#define LCD_PIN_R5  42
#define LCD_PIN_R6  41
#define LCD_PIN_R7  40

// --- I2C (expander CH422G, RTC) ---
#define I2C_PIN_SDA  8
#define I2C_PIN_SCL  9

// --- CH422G : broches EXIO ---
#define EXIO_LCD_BL     2
#define EXIO_LCD_RST    3
#define EXIO_SD_CS      4

// --- Carte SD (SPI, CS via EXIO_SD_CS) ---
#define SD_PIN_MOSI  11
#define SD_PIN_SCK   12
#define SD_PIN_MISO  13

// --- CAN / RS485 : broches confirmées par le schéma (sens TX/RX du CAN d'après le wiki) ---
#define CAN_PIN_TX    15
#define CAN_PIN_RX    16
#define RS485_PIN_TX  44
#define RS485_PIN_RX  43
