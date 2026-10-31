#include "color.h"

#include <math.h>

// Same order and values as the preview: Sunflower, Lagoon, Lime, Glacier, Mint, Seafoam, Cream, Mono.
// The face is always black with neutral gray ticks; only the slice color changes.
#define FACE 0x000000
#define TICK 0x2A2A2A
#define MAJOR 0x4A4A4A
const palette_t PALETTES[PALETTE_COUNT] = {
    {FACE, 0xFFC83D, TICK, MAJOR},
    {FACE, 0x3FE0C5, TICK, MAJOR},
    {FACE, 0xB8E34A, TICK, MAJOR},
    {FACE, 0x6EC6FF, TICK, MAJOR},
    {FACE, 0x8EF0B8, TICK, MAJOR},
    {FACE, 0xA8EFE4, TICK, MAJOR},
    {FACE, 0xF1E3C8, TICK, MAJOR},
    {FACE, 0xFFFFFF, TICK, MAJOR},
};

#define LEUCHTORANGE 0xFF4D06 // RAL 2005, screen approximation
#define DEEP_RED 0xE8143A

static float to_lin(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }
static float to_srgb(float c) { return c <= 0.0031308f ? 12.92f * c : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f; }

lab_t rgb_to_oklab(uint32_t rgb)
{
    const float r = to_lin(((rgb >> 16) & 0xFF) / 255.0f);
    const float g = to_lin(((rgb >> 8) & 0xFF) / 255.0f);
    const float b = to_lin((rgb & 0xFF) / 255.0f);
    const float l = cbrtf(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
    const float m = cbrtf(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
    const float s = cbrtf(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
    return (lab_t){
        0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s,
        1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
        0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s,
    };
}

static uint32_t chan(float c)
{
    c = to_srgb(c < 0 ? 0 : c);
    if (c > 1) c = 1;
    return (uint32_t)lroundf(c * 255.0f);
}

uint32_t oklab_to_rgb(lab_t c)
{
    float l = c.L + 0.3963377774f * c.a + 0.2158037573f * c.b;
    float m = c.L - 0.1055613458f * c.a - 0.0638541728f * c.b;
    float s = c.L - 0.0894841775f * c.a - 1.2914855480f * c.b;
    l = l * l * l;
    m = m * m * m;
    s = s * s * s;
    const float r = 4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
    const float g = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
    const float b = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;
    return chan(r) << 16 | chan(g) << 8 | chan(b);
}

lab_t lab_mix(lab_t p, lab_t q, float t)
{
    return (lab_t){p.L + (q.L - p.L) * t, p.a + (q.a - p.a) * t, p.b + (q.b - p.b) * t};
}

static int crosses_purple(float h, float dh)
{
    for (int k = 1; k < 24; k++) {
        float deg = fmodf((h + dh * k / 24.0f) * 180.0f / (float)M_PI, 360.0f);
        if (deg < 0) deg += 360.0f;
        if (deg > 285.0f && deg < 345.0f) return 1;
    }
    return 0;
}

static lab_t lch_mix(lab_t p, lab_t q, float t)
{
    const float C1 = hypotf(p.a, p.b), C2 = hypotf(q.a, q.b);
    const float h2 = atan2f(q.b, q.a);
    float h1 = atan2f(p.b, p.a);
    if (C1 < 0.02f) h1 = h2; // near-white has no real hue
    float dh = h2 - h1;
    if (dh > (float)M_PI) dh -= 2 * (float)M_PI;
    else if (dh < -(float)M_PI) dh += 2 * (float)M_PI;
    // Avoid sweeping through purple, unless the color already is purple.
    float deg1 = fmodf(h1 * 180.0f / (float)M_PI + 360.0f, 360.0f);
    const int starts_purple = C1 >= 0.02f && deg1 > 285.0f && deg1 < 345.0f;
    if (!starts_purple && crosses_purple(h1, dh)) dh += dh > 0 ? -2 * (float)M_PI : 2 * (float)M_PI;
    const float L = p.L + (q.L - p.L) * t, C = C1 + (C2 - C1) * t, h = h1 + dh * t;
    return (lab_t){L, C * cosf(h), C * sinf(h)};
}

lab_t warm_color(lab_t base, float min)
{
    static lab_t orange, red;
    static int ready;
    if (!ready) {
        orange = rgb_to_oklab(LEUCHTORANGE);
        red = rgb_to_oklab(DEEP_RED);
        ready = 1;
    }
    if (min >= 10) return base;
    if (min >= 5) return lch_mix(base, orange, (10 - min) / 5);
    if (min >= 1) return lch_mix(orange, red, (5 - min) / 4);
    return red;
}
