#pragma once
#include <stdint.h>

#define DISPLAY_BAND_ROWS 24 // rows per DMA transfer; LCD_V_RES must be a multiple

void display_init(void);
void display_set_backlight(uint8_t duty); // 0..255

// Band pipeline: get a free DMA buffer (blocks until one is free), fill
// LCD_H_RES * rows pixels of byte-swapped RGB565, then queue it.
uint16_t *display_band_buffer(void);
void display_flush_band(int y0, int rows);
void display_wait_idle(void);
