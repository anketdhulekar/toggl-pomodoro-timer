#pragma once
#include <stdint.h>

typedef struct {
    uint32_t dial, slice, tick, major; // 0xRRGGBB
} palette_t;

#define PALETTE_COUNT 8
extern const palette_t PALETTES[PALETTE_COUNT];

typedef struct {
    float L, a, b;
} lab_t;

lab_t rgb_to_oklab(uint32_t rgb);
uint32_t oklab_to_rgb(lab_t c);
lab_t lab_mix(lab_t p, lab_t q, float t);

// Profile color until 10 min left, fading to Leuchtorange by 5 min and to
// deep red by 1 min. Fades travel around the OKLCH hue wheel, never through purple.
lab_t warm_color(lab_t base, float minutes_left);

static inline uint16_t rgb_to_565(uint32_t rgb)
{
    const uint32_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    return (uint16_t)(((r * 249 + 1014) >> 11) << 11 | ((g * 253 + 505) >> 10) << 5 | ((b * 249 + 1014) >> 11));
}
