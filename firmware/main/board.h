// Waveshare ESP32-S3-Knob-Touch-LCD-1.8 pin map (from Waveshare's ESP-IDF demos).
#pragma once

#define LCD_H_RES 360
#define LCD_V_RES 360

// 1.8" 360x360 round IPS, QSPI
#define PIN_LCD_CS 14
#define PIN_LCD_PCLK 13
#define PIN_LCD_D0 15
#define PIN_LCD_D1 16
#define PIN_LCD_D2 17
#define PIN_LCD_D3 18
#define PIN_LCD_RST 21
#define PIN_LCD_BL 47

// Shared I2C bus: CST816 touch + DRV2605 haptic driver
#define PIN_I2C_SDA 11
#define PIN_I2C_SCL 12
#define TOUCH_ADDR 0x15
#define PIN_TOUCH_RST 10
#define PIN_TOUCH_INT 9
#define DRV2605_ADDR 0x5A

// Rotary knob: each direction pulses its own line (not quadrature)
#define PIN_KNOB_A 8 // pulses when turned right
#define PIN_KNOB_B 7 // pulses when turned left
