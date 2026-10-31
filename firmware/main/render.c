// Software renderer for the two-hand countdown.
//
// Layers, bottom to top (same as the HTML preview):
//   static dial + ticks  ->  glow  ->  text in the live color  ->  hands  ->
//   slice  ->  text in the dial color, clipped to the slice  ->  hold ring
// The slice, hands and off-slice text share the live color, so they collapse
// into one coverage value per pixel.
//
// Performance notes (ESP32-S3):
// - libm calls (sqrtf, floorf, fminf...) are not inlined on Xtensa, and a
//   loop with many live floats spills to the stack. So each row is drawn in
//   small passes: live coverage (slice, rim, hands) into a byte buffer, then
//   an integer compositing pass, then the hold ring.
// - Only rows touched by a change are redrawn (see dirty_rows).
#include "render.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "assets.h"
#include "board.h"
#include "color.h"
#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"

static const char *TAG = "render";

#define CX 180.0f
#define CY 180.0f
#define HAND_R 170.0f
#define HAND_COV 2.5f // half width (2 px) + 0.5 px anti-aliasing
#define DYN_R 174.0f  // outside this radius only the static layer shows
#define RING_R 128.0f
#define DIGIT_BASELINE 214.0f
#define COLON_BASELINE 210.0f
#define STATUS_BASELINE 248.0f
#define PULSE_CY 182.0f // the pulse swells the digits around this point
#define DOTS_Y 288.0f
// Lines above the digits: one line sits at TEXT_SINGLE; a task with a project
// under it uses TEXT_TOP and TEXT_SECOND.
#define TEXT_SINGLE 122
#define TEXT_TOP 108
#define TEXT_SECOND 128
#define TEXT_ROWS_LO 90  // rows those lines can touch
#define TEXT_ROWS_HI 135
#define TASK_FADE 16 // px: a scrolling task fades out at the window edges
#define MIC_CY ((float)RENDER_MIC_CY)
#define TOP_CY RENDER_TOP_CY
#define TOP_DISC_R 30.5f // black disc behind the badge, covering the hand and slice
#define WIFI_CY 341      // Wi-Fi glyph center, in the rim at 6 o'clock
#define WAVE_CY 248.0f
#define RIM_R 155.0f // project rim: from here to the edge of the screen

// Text layer region: covers the task name, the digits (with pulse swell and glow), status, dots and mic.
#define TX0 10
#define TY0 86
#define TW 340
#define TH 244

static const float SLOT_X[4] = {80, 138, 222, 280};

typedef struct {
    uint8_t *px;                // TW x TH coverage
    int16_t lo[TH], hi[TH];     // per-row written x range (layer coords), lo > hi = empty
} layer_t;

static uint16_t *s_bg; // static layer, byte-swapped RGB565
static layer_t s_text, s_glow;
static uint64_t s_bg_key = UINT64_MAX; // face/tick colors the static layer was built with
static bool s_rot180;
static scene_t s_last;
static bool s_has_last;
static render_stats_t s_stats;
static uint8_t s_cov[LCD_H_RES]; // live coverage for the current row

// Per-frame constants
static struct {
    bool empty, full, convex, glow_on, ring_on;
    int top_a256, wifi_a256; // overlay strengths (0 = off)
    uint16_t wifi_c;         // Wi-Fi glyph color (a tick color)
    float bx, by, ring_end;
    uint16_t live, dial, ring; // native RGB565
} F;

static inline uint16_t swap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
static inline float minf(float a, float b) { return a < b ? a : b; }
static inline float maxf(float a, float b) { return a > b ? a : b; }
static inline float absf(float v) { return v < 0 ? -v : v; }
static inline int ifloor(float v) { return (int)(v + 1024.0f) - 1024; } // floor for |v| < 1024, no libcall
static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

// Fast blend of native RGB565 colors with a 0..256 alpha (5-bit precision).
static inline uint16_t blend565(uint16_t bg, uint16_t fg, uint32_t a)
{
    const uint32_t alpha = (a + 4) >> 3;
    uint32_t b = (bg | ((uint32_t)bg << 16)) & 0x07E0F81Fu;
    const uint32_t f = (fg | ((uint32_t)fg << 16)) & 0x07E0F81Fu;
    b += ((f - b) * alpha) >> 5;
    b &= 0x07E0F81Fu;
    return (uint16_t)(b | (b >> 16));
}

// Per-channel blend with a 0..256 alpha (8-bit precision), for text edges.
static inline uint16_t mix565(uint16_t a, uint16_t b, int t)
{
    int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31;
    const int br = b >> 11, bgn = (b >> 5) & 63, bb = b & 31;
    ar += ((br - ar) * t + 128) >> 8;
    ag += ((bgn - ag) * t + 128) >> 8;
    ab += ((bb - ab) * t + 128) >> 8;
    return (uint16_t)(ar << 11 | ag << 5 | ab);
}

void render_take_stats(render_stats_t *out)
{
    *out = s_stats;
    memset(&s_stats, 0, sizeof s_stats);
}

// ---- Static layer (rebuilt only when the color profile changes) -----------

static void build_bg(int pal, bool rim_on, uint32_t rim_rgb, bool no_six)
{
    const palette_t *p = &PALETTES[pal];
    const uint16_t dial = rgb_to_565(p->dial), rim = rgb_to_565(rim_rgb);
    // On a project rim the ticks become dark notches.
    const uint16_t tick = rim_on ? blend565(rim, 0, 108) : rgb_to_565(p->tick);
    const uint16_t major = rim_on ? blend565(rim, 0, 159) : rgb_to_565(p->major);
    float ux[12], uy[12];
    for (int k = 0; k < 12; k++) {
        ux[k] = sinf(k * (float)M_PI / 6);
        uy[k] = -cosf(k * (float)M_PI / 6);
    }
    for (int y = 0; y < LCD_V_RES; y++) {
        const float dy = y + 0.5f - CY;
        for (int x = 0; x < LCD_H_RES; x++) {
            const float dx = x + 0.5f - CX;
            const float r2 = dx * dx + dy * dy;
            uint16_t c = dial;
            if (rim_on && r2 > (RIM_R - 0.5f) * (RIM_R - 0.5f)) {
                const float band = clamp01(sqrtf(r2) - (RIM_R - 0.5f)); // anti-aliased inner edge
                c = band >= 1 ? rim : blend565(dial, rim, (uint32_t)(band * 256));
            }
            if (r2 > 154.0f * 154.0f && r2 < 172.0f * 172.0f) {
                float phi = atan2f(dx, -dy);
                if (phi < 0) phi += 2 * (float)M_PI;
                const int k = (int)lroundf(phi * (6 / (float)M_PI)) % 12;
                if (no_six && k == 6) {
                    s_bg[y * LCD_H_RES + x] = swap16(c);
                    continue;
                }
                float t = dx * ux[k] + dy * uy[k];
                t = t < 158 ? 158 : (t > 168 ? 168 : t);
                const float px = dx - t * ux[k], py = dy - t * uy[k];
                const bool is_major = k % 3 == 0;
                const float cov = clamp01((is_major ? 1.5f : 1.0f) + 0.5f - sqrtf(px * px + py * py));
                if (cov > 0) c = blend565(c, is_major ? major : tick, (uint32_t)(cov * 256));
            }
            s_bg[y * LCD_H_RES + x] = swap16(c);
        }
    }
}

static uint64_t bg_key(const scene_t *s)
{
    const palette_t *p = &PALETTES[s->palette];
    const uint64_t rim = s->rim_on ? (s->rim_rgb | 0x1000000u) : 0;
    return rim << 39 ^ (uint64_t)p->dial << 24 ^ (uint64_t)p->tick << 12 ^ p->major ^ (uint64_t)s->wifi_rim << 63;
}

// ---- Text layers ------------------------------------------------------------

static void layer_clear(layer_t *L)
{
    for (int r = 0; r < TH; r++) {
        if (L->hi[r] >= L->lo[r]) memset(L->px + r * TW + L->lo[r], 0, L->hi[r] - L->lo[r] + 1);
        L->lo[r] = TW;
        L->hi[r] = -1;
    }
}

static inline void layer_mark(layer_t *L, int Y, int X0, int X1) // absolute coords, X1 exclusive
{
    const int r = Y - TY0;
    if (X0 - TX0 < L->lo[r]) L->lo[r] = (int16_t)(X0 - TX0);
    if (X1 - 1 - TX0 > L->hi[r]) L->hi[r] = (int16_t)(X1 - 1 - TX0);
}

static inline int mask_px(const mask_t *m, int x, int y)
{
    return ((unsigned)x >= m->w || (unsigned)y >= m->h) ? 0 : m->data[y * m->w + x];
}

// Draw a mask anchored at (ax, ay), scaled by sc around (scx, scy); overlapping
// coverage keeps the max. sc == 1 is a straight copy; otherwise bilinear.
static void blit(layer_t *L, const mask_t *m, float ax, float ay, float sc, float scx, float scy, float alpha)
{
    if (alpha <= 0.004f) return;
    const int a256 = (int)(alpha * 256 + 0.5f);
    const float left = ax + m->ox, top = ay + m->oy;

    if (sc > 0.9999f && sc < 1.0001f) {
        const int lx = ifloor(left + 0.5f), ty = ifloor(top + 0.5f);
        const int mx0 = imax(0, TX0 - lx), mx1 = imin(m->w, TX0 + TW - lx);
        if (mx1 <= mx0) return;
        for (int my = 0; my < m->h; my++) {
            const int Y = ty + my;
            if (Y < TY0 || Y >= TY0 + TH) continue;
            uint8_t *row = L->px + (Y - TY0) * TW + (lx - TX0);
            const uint8_t *src = m->data + my * m->w;
            for (int mx = mx0; mx < mx1; mx++) {
                const int val = (src[mx] * a256) >> 8;
                if (val > row[mx]) row[mx] = (uint8_t)val;
            }
            layer_mark(L, Y, lx + mx0, lx + mx1);
        }
        return;
    }

    const int x0 = imax(TX0, ifloor(scx + (left - scx) * sc) - 1);
    const int x1 = imin(TX0 + TW, ifloor(scx + (left + m->w - scx) * sc) + 2);
    const int y0 = imax(TY0, ifloor(scy + (top - scy) * sc) - 1);
    const int y1 = imin(TY0 + TH, ifloor(scy + (top + m->h - scy) * sc) + 2);
    if (x1 <= x0) return;
    const float inv = 1.0f / sc;
    const float u0 = scx + (x0 + 0.5f - scx) * inv - left - 0.5f;
    for (int Y = y0; Y < y1; Y++) {
        const float v = scy + (Y + 0.5f - scy) * inv - top - 0.5f;
        const int vi = ifloor(v);
        if (vi < -1 || vi >= m->h) continue;
        const int fv = (int)((v - vi) * 256);
        uint8_t *row = L->px + (Y - TY0) * TW - TX0;
        float u = u0;
        for (int X = x0; X < x1; X++, u += inv) {
            const int ui = ifloor(u);
            if (ui < -1 || ui >= m->w) continue;
            const int fu = (int)((u - ui) * 256);
            const int top_s = mask_px(m, ui, vi) * (256 - fu) + mask_px(m, ui + 1, vi) * fu;
            const int bot_s = mask_px(m, ui, vi + 1) * (256 - fu) + mask_px(m, ui + 1, vi + 1) * fu;
            int val = (int)(((int64_t)(top_s * (256 - fv) + bot_s * fv) * a256) >> 24);
            if (val > 255) val = 255;
            if (val > row[X]) row[X] = (uint8_t)val;
        }
        layer_mark(L, Y, x0, x1);
    }
}

static void dot(layer_t *L, float cx, float cy, float r, float alpha)
{
    const int X0 = (int)(cx - r - 1), X1 = (int)(cx + r + 2);
    for (int Y = (int)(cy - r - 1); Y <= (int)(cy + r + 1); Y++) {
        for (int X = X0; X < X1; X++) {
            const float dx = X + 0.5f - cx, dy = Y + 0.5f - cy;
            const int val = (int)(clamp01(r + 0.5f - sqrtf(dx * dx + dy * dy)) * alpha * 255 + 0.5f);
            uint8_t *p = L->px + (Y - TY0) * TW + (X - TX0);
            if (val > *p) *p = (uint8_t)val;
        }
        layer_mark(L, Y, X0, X1);
    }
}

// ---- Task name ---------------------------------------------------------------

// Decode UTF-8 into task-font indices. Accented Latin letters lose their accents,
// typographic quotes and dashes become ASCII, anything else (emoji...) is dropped.
static int task_glyphs(const char *s, uint8_t *out, int cap)
{
    static const char LATIN1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty"; // U+00C0..U+00FF
    int n = 0;
    const uint8_t *p = (const uint8_t *)s;
    while (*p && n < cap) {
        uint32_t cp = *p++;
        int extra = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
        if (extra) cp &= 0x3F >> extra;
        for (; extra > 0 && (*p & 0xC0) == 0x80; extra--) cp = cp << 6 | (*p++ & 0x3F);
        int ch = -1;
        if (cp >= 32 && cp < 127) ch = (int)cp;
        else if (cp == 0xA0 || cp == '\t' || cp == '\n') ch = ' ';
        else if (cp >= 0xC0 && cp <= 0xFF) ch = LATIN1[cp - 0xC0];
        else if (cp == 0x2018 || cp == 0x2019 || cp == 0x201A || cp == 0x2032) ch = '\'';
        else if (cp == 0x201C || cp == 0x201D || cp == 0x201E || cp == 0x2033) ch = '"';
        else if (cp == 0x2013 || cp == 0x2014 || cp == 0x2212) ch = '-';
        else if (cp == 0x2026) {
            out[n++] = TASK_FONT_ELLIPSIS;
            continue;
        }
        if (ch >= 0) out[n++] = (uint8_t)(ch - TASK_FONT_FIRST);
    }
    return n;
}

// Glyphs for `text` in `font`, cut with an ellipsis to fit max16 (1/16 px; 0 = no limit).
// Returns the count and the width.
static int layout_text(const char *text, const glyph_t *font, uint8_t *g, int max16, int *w16_out)
{
    int n = task_glyphs(text, g, SCENE_TASK_LEN - 1);
    int w16 = 0;
    for (int i = 0; i < n; i++) w16 += font[g[i]].adv16;
    if (max16 && w16 > max16) { // cut at a glyph boundary, drop trailing spaces, add an ellipsis
        const int ell = font[TASK_FONT_ELLIPSIS].adv16;
        while (n > 0 && (w16 + ell > max16 || g[n - 1] == ' ' - TASK_FONT_FIRST)) w16 -= font[g[--n]].adv16;
        g[n++] = TASK_FONT_ELLIPSIS;
        w16 += ell;
    }
    *w16_out = w16;
    return n;
}

int render_task_width(const char *task)
{
    uint8_t g[SCENE_TASK_LEN];
    int w16;
    layout_text(task, TASK_FONT, g, 0, &w16);
    return (w16 + 15) / 16;
}

// One line of text centered on the screen (cut with an ellipsis if too wide).
static void draw_line(layer_t *L, const char *text, const glyph_t *font, int baseline, float alpha)
{
    uint8_t g[SCENE_TASK_LEN];
    int w16;
    const int n = layout_text(text, font, g, TASK_MAX_W * 16, &w16);
    int pen16 = (int)(CX * 16) - w16 / 2;
    for (int i = 0; i < n; i++) {
        const glyph_t *gl = &font[g[i]];
        if (gl->m.w) blit(L, &gl->m, (float)((pen16 + 8) >> 4), (float)baseline, 1.0f, CX, PULSE_CY, alpha);
        pen16 += gl->adv16;
    }
}

// A glyph clipped to the window [wx0, wx1), fading out over TASK_FADE px at each edge.
static void blit_window(layer_t *L, const mask_t *m, int pen_x, int baseline, int a256, int wx0, int wx1)
{
    const int lx = pen_x + m->ox, ty = baseline + m->oy;
    const int mx0 = imax(0, wx0 - lx), mx1 = imin(m->w, wx1 - lx);
    if (mx1 <= mx0) return;
    for (int my = 0; my < m->h; my++) {
        const int Y = ty + my;
        if (Y < TY0 || Y >= TY0 + TH) continue;
        uint8_t *row = L->px + (Y - TY0) * TW - TX0;
        const uint8_t *src = m->data + my * m->w;
        for (int mx = mx0; mx < mx1; mx++) {
            const int X = lx + mx, edge = imin(X - wx0, wx1 - 1 - X);
            const int f = edge >= TASK_FADE ? 256 : (edge * 256) / TASK_FADE;
            const int val = ((src[mx] * a256) >> 8) * f >> 8;
            if (val > row[X]) row[X] = (uint8_t)val;
        }
        layer_mark(L, Y, lx + mx0, lx + mx1);
    }
}

// A task too wide for the screen scrolls through a window, in a loop with a gap.
static void draw_marquee(layer_t *L, const char *text, int baseline, float alpha, int scroll)
{
    uint8_t g[SCENE_TASK_LEN];
    int w16;
    const int n = layout_text(text, TASK_FONT, g, 0, &w16);
    const int wx0 = (int)CX - TASK_MAX_W / 2, wx1 = wx0 + TASK_MAX_W;
    const int a256 = (int)(alpha * 256 + 0.5f);
    for (int copy = 0; copy < 2; copy++) {
        int pen16 = (wx0 - scroll + copy * ((w16 + 15) / 16 + TASK_SCROLL_GAP)) * 16;
        for (int i = 0; i < n; i++) {
            const glyph_t *gl = &TASK_FONT[g[i]];
            if (gl->m.w) blit_window(L, &gl->m, (pen16 + 8) >> 4, baseline, a256, wx0, wx1);
            pen16 += gl->adv16;
        }
    }
}

// A vertical 4 px capsule centered at (cx, cy), h px tall, into the layer.
static void bar(layer_t *L, float cx, float cy, int h)
{
    const float half = h < 4 ? 0 : (h - 4) / 2.0f; // distance from the center to each cap's center
    const int X0 = (int)cx - 3, X1 = (int)cx + 4;
    const int Y0 = imax(TY0, (int)(cy - half) - 3), Y1 = imin(TY0 + TH - 1, (int)(cy + half) + 3);
    for (int Y = Y0; Y <= Y1; Y++) {
        const float py = Y + 0.5f - cy;
        const float dy = py > half ? py - half : (py < -half ? py + half : 0);
        for (int X = X0; X < X1; X++) {
            const float dx = X + 0.5f - cx;
            const int val = (int)(clamp01(2.5f - sqrtf(dx * dx + dy * dy)) * 255);
            uint8_t *p = L->px + (Y - TY0) * TW + (X - TX0);
            if (val > *p) *p = (uint8_t)val;
        }
        layer_mark(L, Y, X0, X1);
    }
}

static void build_text(const scene_t *s)
{
    layer_clear(&s_text);
    for (int i = 0; i < 4; i++) {
        blit(&s_text, &GLYPH_DIGITS[s->digits[i] - '0'], SLOT_X[i], DIGIT_BASELINE, s->digit_scale, CX, PULSE_CY,
             s->digit_alpha);
    }
    blit(&s_text, &GLYPH_COLON, CX, COLON_BASELINE, s->digit_scale, CX, PULSE_CY, s->digit_alpha * s->colon_alpha);
    if (s->task[0]) {
        const int base = s->project[0] ? TEXT_TOP : TEXT_SINGLE;
        if (s->task_scroll >= 0) draw_marquee(&s_text, s->task, base, s->task_alpha, s->task_scroll);
        else draw_line(&s_text, s->task, TASK_FONT, base, s->task_alpha);
    }
    if (s->project[0]) draw_line(&s_text, s->project, SMALL_FONT, s->task[0] ? TEXT_SECOND : TEXT_SINGLE, s->project_alpha);
    if (s->status >= 0 && s->status < STR_COUNT) {
        blit(&s_text, &STRINGS[s->status], CX, STATUS_BASELINE, 1.0f, CX, PULSE_CY, s->status_alpha);
    }
    if (s->mic_ring_alpha > 0.004f) blit(&s_text, &MIC_RING, CX, MIC_CY, 1.0f, CX, PULSE_CY, s->mic_ring_alpha);
    if (s->mic_alpha > 0.004f) blit(&s_text, &MIC_ICON, CX, MIC_CY, 1.0f, CX, PULSE_CY, s->mic_alpha);
    if (s->wave_on) { // 21 rounded bars, 4 px wide, 7 px apart
        for (int i = 0; i < 21; i++) bar(&s_text, CX + (i - 10) * 7, WAVE_CY, s->wave[i]);
    }
    if (s->dots_alpha > 0.004f) {
        for (int i = 0; i < PALETTE_COUNT; i++) {
            dot(&s_text, CX + (i - (PALETTE_COUNT - 1) / 2.0f) * 14, DOTS_Y, 3.5f,
                s->dots_alpha * (i == s->palette ? 1.0f : 0.35f));
        }
    }
    // The glow is blurred, so it is drawn at rest size; only its strength pulses.
    layer_clear(&s_glow);
    if (s->glow > 0.004f) {
        const float g = s->glow * s->digit_alpha;
        for (int i = 0; i < 4; i++) {
            blit(&s_glow, &GLOW_DIGITS[s->digits[i] - '0'], SLOT_X[i], DIGIT_BASELINE, 1.0f, CX, PULSE_CY, g);
        }
        blit(&s_glow, &GLOW_COLON, CX, COLON_BASELINE, 1.0f, CX, PULSE_CY, g);
    }
}

// ---- Dirty rows -------------------------------------------------------------

// Rows covered by the arc between angles a1..a2 (degrees clockwise from 12)
// out to radius r, including the center.
static void arc_rows(float a1, float a2, float r, int *lo, int *hi)
{
    if (a1 > a2) {
        const float t = a1;
        a1 = a2;
        a2 = t;
    }
    float ymin = CY, ymax = CY;
    const float ys[2] = {CY - r * cosf(a1 * (float)M_PI / 180), CY - r * cosf(a2 * (float)M_PI / 180)};
    for (int i = 0; i < 2; i++) {
        ymin = minf(ymin, ys[i]);
        ymax = maxf(ymax, ys[i]);
    }
    if (a1 <= 0.0f || a2 >= 360.0f) ymin = CY - r; // passes 12 o'clock
    if (a1 < 180.0f && a2 > 180.0f) ymax = CY + r; // passes 6 o'clock
    *lo = ifloor(ymin) - 4;
    *hi = ifloor(ymax) + 5;
}

static void add_rows(int *ya, int *yb, int lo, int hi)
{
    *ya = imin(*ya, lo);
    *yb = imax(*yb, hi);
}

static void dirty_rows(const scene_t *s, int *ya, int *yb)
{
    const scene_t *p = &s_last;
    *ya = LCD_V_RES;
    *yb = 0;
    if (!s_has_last || s->palette != p->palette || rgb_to_565(s->live_rgb) != rgb_to_565(p->live_rgb) ||
        s->rim_on != p->rim_on || s->rim_rgb != p->rim_rgb) {
        *ya = 0;
        *yb = LCD_V_RES;
        return;
    }
    if (s->slice_deg != p->slice_deg) {
        int lo, hi;
        arc_rows(p->slice_deg, s->slice_deg, DYN_R, &lo, &hi);
        add_rows(ya, yb, lo, hi);
    }
    if (s->hold_progress != p->hold_progress) {
        int lo, hi;
        arc_rows(p->hold_progress * 360, s->hold_progress * 360, RING_R + 3, &lo, &hi);
        add_rows(ya, yb, lo, hi);
    }
    if (memcmp(s->digits, p->digits, 4) || s->digit_alpha != p->digit_alpha || s->colon_alpha != p->colon_alpha ||
        s->digit_scale != p->digit_scale || s->glow != p->glow) {
        add_rows(ya, yb, 104, 252);
    }
    if (s->status != p->status || s->status_alpha != p->status_alpha) add_rows(ya, yb, 232, 258);
    if (s->mic_alpha != p->mic_alpha || s->mic_ring_alpha != p->mic_ring_alpha) add_rows(ya, yb, 262, 330);
    if (s->wave_on != p->wave_on || memcmp(s->wave, p->wave, sizeof s->wave)) add_rows(ya, yb, 232, 264);
    if (s->top_alpha != p->top_alpha) add_rows(ya, yb, TOP_CY - 36, TOP_CY + 37);
    if (s->wifi_alpha != p->wifi_alpha) add_rows(ya, yb, WIFI_CY - 13, WIFI_CY + 13);
    if (s->task_alpha != p->task_alpha || s->task_scroll != p->task_scroll || strcmp(s->task, p->task) ||
        s->project_alpha != p->project_alpha || strcmp(s->project, p->project)) {
        add_rows(ya, yb, TEXT_ROWS_LO, TEXT_ROWS_HI);
    }
    if (s->dots_alpha != p->dots_alpha) add_rows(ya, yb, 280, 298);
    *ya = imax(0, *ya);
    *yb = imin(LCD_V_RES, *yb);
}

// ---- Row passes -------------------------------------------------------------

// Live-color coverage (slice with its rim, plus both hands) for one row.
static void row_coverage(int y, int xa, int xb)
{
    uint8_t *cov = s_cov;
    memset(cov + xa, 0, xb - xa);
    const float dy = y + 0.5f - CY;

    if (!F.empty) {
        const float out2 = 150.5f * 150.5f - dy * dy;
        if (out2 > 0) {
            const float out_half = sqrtf(out2);
            const int xs0 = imax(xa, ifloor(CX - out_half)), xs1 = imin(xb, ifloor(CX + out_half) + 1);
            const int mid = 180; // the 12 o'clock edge lies between columns 179 and 180
            if (F.full) {
                memset(cov + xs0, 255, xs1 - xs0);
            } else {
                // Before the moving hand: raw = signed distance to its line, linear along x.
                const float by = F.by;
                const int c0 = F.convex ? imax(xs0, mid) : xs0;
                const int c1 = F.convex ? xs1 : imin(xs1, mid);
                float raw = (c0 + 0.5f - CX) * by - dy * F.bx + 0.5f;
                for (int x = c0; x < c1; x++, raw += by) {
                    const int v = (int)(raw * 255.0f);
                    cov[x] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
                }
                if (!F.convex && xs1 > mid) memset(cov + imax(mid, xs0), 255, xs1 - imax(mid, xs0));
            }
            // Rim anti-aliasing: only the outermost pixels on each side.
            const float in2 = 149.5f * 149.5f - dy * dy;
            const float in_half = in2 > 0 ? sqrtf(in2) : 0.0f;
            const int xi0 = imax(xs0, ifloor(CX - in_half) + 1), xi1 = imin(xs1, ifloor(CX + in_half));
            for (int x = xs0; x < xs1; x++) {
                if (x == xi0 && xi1 > xi0) {
                    x = xi1 - 1;
                    continue;
                }
                if (!cov[x]) continue;
                const float dx = x + 0.5f - CX;
                const float cr = clamp01(150.5f - sqrtf(dx * dx + dy * dy));
                cov[x] = (uint8_t)(cov[x] * cr + 0.5f);
            }
        }
    }

    // 12 o'clock hand: columns 177..182, from the center up to radius 170.
    if (dy > -HAND_R - HAND_COV && dy < HAND_COV) {
        for (int x = 177; x <= 182; x++) {
            const float dx = x + 0.5f - CX;
            float d = absf(dx);
            if (dy > 0) d = sqrtf(dx * dx + dy * dy);
            else if (dy < -HAND_R) d = sqrtf(dx * dx + (dy + HAND_R) * (dy + HAND_R));
            const int h = (int)(clamp01(HAND_COV - d) * 255.0f);
            if (h) cov[x] = (uint8_t)(255 - ((255 - cov[x]) * (255 - h) + 127) / 255);
        }
    }

    // Moving hand: pixels within 2.5 px of its line and along its length.
    {
        const float bx = F.bx, by = F.by;
        const float raw0 = (xa + 0.5f - CX) * by - dy * bx; // at x = xa
        const float tb0 = (xa + 0.5f - CX) * bx + dy * by;
        float lo = 0, hi = (float)(xb - xa); // offsets from xa
        if (absf(by) > 1e-4f) {
            const float e0 = (-HAND_COV - raw0) / by, e1 = (HAND_COV - raw0) / by;
            lo = maxf(lo, minf(e0, e1));
            hi = minf(hi, maxf(e0, e1));
        } else if (absf(raw0) >= HAND_COV) {
            hi = -1;
        }
        if (absf(bx) > 1e-4f) {
            const float e0 = (-HAND_COV - tb0) / bx, e1 = (HAND_R + HAND_COV - tb0) / bx;
            lo = maxf(lo, minf(e0, e1));
            hi = minf(hi, maxf(e0, e1));
        } else if (tb0 <= -HAND_COV || tb0 >= HAND_R + HAND_COV) {
            hi = -1;
        }
        const int x0 = xa + imax(0, ifloor(lo)), x1 = imin(xb, xa + ifloor(hi) + 2);
        for (int x = x0; x < x1; x++) {
            const float dx = x + 0.5f - CX;
            const float raw = dx * by - dy * bx, tb = dx * bx + dy * by;
            float d = absf(raw);
            if (tb < 0) d = sqrtf(dx * dx + dy * dy);
            else if (tb > HAND_R) {
                const float px = dx - HAND_R * bx, py = dy - HAND_R * by;
                d = sqrtf(px * px + py * py);
            }
            const int h = (int)(clamp01(HAND_COV - d) * 255.0f);
            if (h) cov[x] = (uint8_t)(255 - ((255 - cov[x]) * (255 - h) + 127) / 255);
        }
    }
}

// Simple pixels: live coverage over the static layer.
static void composite_plain(uint16_t *o, const uint16_t *bg, int x0, int x1)
{
    const uint8_t *cov = s_cov;
    const uint16_t live = F.live, live_sw = swap16(F.live);
    for (int x = x0; x < x1; x++) {
        const int c = cov[x];
        o[x] = c == 0 ? bg[x] : (c == 255 ? live_sw : swap16(blend565(swap16(bg[x]), live, (uint32_t)(c + (c >> 7)))));
    }
}

// Pixels under text and glow: glow, then live text, then dial-color text inside the slice.
static void composite_text(uint16_t *o, const uint16_t *bg, int x0, int x1, const uint8_t *trow, const uint8_t *grow)
{
    const uint8_t *cov = s_cov;
    const uint16_t live = F.live, dial = F.dial;
    for (int x = x0; x < x1; x++) {
        const int t = trow[x], g = grow ? grow[x] : 0, c = cov[x];
        if ((t | g) == 0) {
            o[x] = c == 0 ? bg[x] : (c == 255 ? swap16(live) : swap16(blend565(swap16(bg[x]), live, (uint32_t)(c + (c >> 7)))));
            continue;
        }
        uint16_t px = swap16(bg[x]);
        if (g) px = mix565(px, live, g + (g >> 7));
        const int l = 255 - ((255 - t) * (255 - c) + 127) / 255;
        if (l) px = mix565(px, live, l + (l >> 7));
        const int ts = (t * c + 127) / 255;
        if (ts) px = mix565(px, dial, ts + (ts >> 7));
        o[x] = swap16(px);
    }
}

static void composite_ring(uint16_t *o, int y)
{
    const float dy = y + 0.5f - CY;
    const float o2 = 130.5f * 130.5f - dy * dy;
    if (o2 <= 0) return;
    const float i2 = 125.5f * 125.5f - dy * dy;
    const float oh = sqrtf(o2), ih = i2 > 0 ? sqrtf(i2) : 0.0f;
    const int spans[2][2] = {{ifloor(CX - oh), ifloor(CX - ih) + 1}, {ifloor(CX + ih), ifloor(CX + oh) + 1}};
    for (int k = 0; k < 2; k++) {
        for (int x = imax(0, spans[k][0]); x < imin(LCD_H_RES, spans[k][1]); x++) {
            const float dx = x + 0.5f - CX;
            float phi = atan2f(dx, -dy);
            if (phi < 0) phi += 2 * (float)M_PI;
            if (phi > F.ring_end) continue;
            const float rg = clamp01(2.0f - absf(sqrtf(dx * dx + dy * dy) - RING_R)) * 0.8f;
            if (rg > 0) o[x] = swap16(mix565(swap16(o[x]), F.ring, (int)(rg * 256)));
        }
        if (ih == 0) break; // single span through the top/bottom of the ring
    }
}

// The recent-entries badge at 12 o'clock: a black disc over the hand and slice,
// with the ring and clock icon on it in the live color.
static void composite_top(uint16_t *o, int y)
{
    const float dy = y + 0.5f - TOP_CY;
    const int my = y - (TOP_CY + HISTORY_ICON.oy);
    for (int x = (int)CX - 36; x < (int)CX + 36; x++) {
        const float dx = x + 0.5f - CX;
        uint16_t px = swap16(o[x]);
        // The disc stays solid whenever the badge shows (only the icon dims), so the hand never shows through.
        const float disc = clamp01(TOP_DISC_R + 0.5f - sqrtf(dx * dx + dy * dy));
        if (disc > 0) px = mix565(px, F.dial, (int)(disc * 256));
        const int mx = x - ((int)CX + HISTORY_ICON.ox);
        const int ring = mask_px(&MIC_RING, mx, my) * 115 >> 8, icon = mask_px(&HISTORY_ICON, mx, my) * 230 >> 8;
        const int cov = (ring > icon ? ring : icon) * F.top_a256 >> 8;
        if (cov) px = mix565(px, F.live, cov + (cov >> 7));
        o[x] = swap16(px);
    }
}

// The Wi-Fi glyph in the rim at 6 o'clock, in the tick color.
static void composite_wifi(uint16_t *o, int y)
{
    const int my = y - (WIFI_CY + WIFI_ICON.oy);
    for (int mx = 0; mx < WIFI_ICON.w; mx++) {
        const int cov = mask_px(&WIFI_ICON, mx, my) * F.wifi_a256 >> 8;
        if (!cov) continue;
        const int x = (int)CX + WIFI_ICON.ox + mx;
        o[x] = swap16(mix565(swap16(o[x]), F.wifi_c, cov + (cov >> 7)));
    }
}

static void render_row(uint16_t *o, int y)
{
    const uint16_t *bg = s_bg + y * LCD_H_RES;
    const float dy = y + 0.5f - CY;
    const float span2 = DYN_R * DYN_R - dy * dy;
    if (span2 <= 0) {
        memcpy(o, bg, LCD_H_RES * sizeof(uint16_t));
        return;
    }
    const float span = sqrtf(span2);
    const int xa = imax(0, ifloor(CX - span)), xb = imin(LCD_H_RES, ifloor(CX + span) + 1);
    memcpy(o, bg, xa * sizeof(uint16_t));
    memcpy(o + xb, bg + xb, (LCD_H_RES - xb) * sizeof(uint16_t));

    row_coverage(y, xa, xb);

    int t0 = xb, t1 = xa; // text/glow x range in this row
    const uint8_t *trow = NULL, *grow = NULL;
    if (y >= TY0 && y < TY0 + TH) {
        const int r = y - TY0;
        if (s_text.hi[r] >= s_text.lo[r]) {
            t0 = s_text.lo[r] + TX0;
            t1 = s_text.hi[r] + TX0 + 1;
        }
        if (F.glow_on && s_glow.hi[r] >= s_glow.lo[r]) {
            t0 = imin(t0, s_glow.lo[r] + TX0);
            t1 = imax(t1, s_glow.hi[r] + TX0 + 1);
            grow = s_glow.px + r * TW - TX0;
        }
        trow = s_text.px + r * TW - TX0;
        t0 = imax(t0, xa);
        t1 = imin(t1, xb);
    }
    if (t1 > t0) {
        composite_plain(o, bg, xa, t0);
        composite_text(o, bg, t0, t1, trow, grow);
        composite_plain(o, bg, t1, xb);
    } else {
        composite_plain(o, bg, xa, xb);
    }
    if (F.top_a256 && y >= TOP_CY - 36 && y < TOP_CY + 36) composite_top(o, y);
    if (F.wifi_a256 && y >= WIFI_CY - 12 && y < WIFI_CY + 12) composite_wifi(o, y);
    if (F.ring_on) composite_ring(o, y);
}

// ---- Frame --------------------------------------------------------------------

void render_init(void)
{
    s_bg = heap_caps_malloc(LCD_H_RES * LCD_V_RES * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    // PSRAM too: the per-row reads are sequential and cache well, and internal RAM is
    // needed by Wi-Fi and TLS (uploads slowed down badly with less than ~20 KB free).
    s_text.px = heap_caps_calloc(1, TW * TH, MALLOC_CAP_SPIRAM);
    // The glow only shows during the last-minute pulse; it lives in PSRAM so
    // internal RAM stays free for Wi-Fi and TLS.
    s_glow.px = heap_caps_calloc(1, TW * TH, MALLOC_CAP_SPIRAM);
    assert(s_bg && s_text.px && s_glow.px);
    for (int r = 0; r < TH; r++) {
        s_text.lo[r] = s_glow.lo[r] = TW;
        s_text.hi[r] = s_glow.hi[r] = -1;
    }
    ESP_LOGI(TAG, "layers ready (text in %s)", esp_ptr_internal(s_text.px) ? "internal RAM" : "PSRAM");
}

void render_invalidate(void) { s_has_last = false; }

void render_set_rotation(int deg)
{
    s_rot180 = (deg % 360) == 180;
    s_has_last = false;
}

static void reverse_row(uint16_t *o)
{
    for (int i = 0, j = LCD_H_RES - 1; i < j; i++, j--) {
        const uint16_t t = o[i];
        o[i] = o[j];
        o[j] = t;
    }
}

// Layers and per-frame constants for a scene.
static void frame_setup(const scene_t *s)
{
    if (bg_key(s) != s_bg_key) {
        build_bg(s->palette, s->rim_on, s->rim_rgb, s->wifi_rim);
        s_bg_key = bg_key(s);
    }
    build_text(s);
    F.live = rgb_to_565(s->live_rgb);
    F.dial = rgb_to_565(PALETTES[s->palette].dial);
    F.ring = rgb_to_565(0x8E8E94);
    F.glow_on = s->glow > 0.004f;
    F.empty = s->slice_deg < 0.05f;
    F.full = s->slice_deg > 359.95f;
    F.convex = s->slice_deg <= 180.0f;
    const float th = s->slice_deg * (float)M_PI / 180.0f;
    F.bx = sinf(th);
    F.by = -cosf(th);
    F.top_a256 = (int)(s->top_alpha * 256);
    F.wifi_a256 = s->wifi_rim ? (int)(s->wifi_alpha * 256) : 0;
    F.wifi_c = s->rim_on ? blend565(rgb_to_565(s->rim_rgb), 0, 159) : rgb_to_565(PALETTES[s->palette].major);
    F.ring_end = s->hold_progress * 2 * (float)M_PI;
    F.ring_on = F.ring_end > 0;
}

void render_frame(const scene_t *s)
{
    const int64_t t0 = esp_timer_get_time();
    int ya, yb;
    dirty_rows(s, &ya, &yb);
    frame_setup(s);
    s_stats.text_us += esp_timer_get_time() - t0;
    s_stats.frames++;

    // Bands are in screen rows; with a 180° rotation, screen row Y shows scene row 359 - Y mirrored.
    for (int y0 = 0; y0 < LCD_V_RES; y0 += DISPLAY_BAND_ROWS) {
        const int s0 = s_rot180 ? LCD_V_RES - (y0 + DISPLAY_BAND_ROWS) : y0;
        if (s0 + DISPLAY_BAND_ROWS <= ya || s0 >= yb) continue; // band unchanged
        const int64_t w0 = esp_timer_get_time();
        uint16_t *out = display_band_buffer();
        s_stats.wait_us += esp_timer_get_time() - w0;
        for (int yy = 0; yy < DISPLAY_BAND_ROWS; yy++) {
            uint16_t *o = out + yy * LCD_H_RES;
            const int Y = y0 + yy;
            render_row(o, s_rot180 ? LCD_V_RES - 1 - Y : Y);
            if (s_rot180) reverse_row(o);
        }
        display_flush_band(y0, DISPLAY_BAND_ROWS);
        s_stats.bands++;
    }
    s_last = *s;
    s_has_last = true;
    s_stats.total_us += esp_timer_get_time() - t0;
}

// ---- Project picker -------------------------------------------------------------
// A list on black: each row is a color dot and a name, centered together. The
// selected row sits in the middle; the others fade with distance.

#define PK_ROW 44.0f
#define PK_MAX_W 240 // px; longer names end in an ellipsis
#define PK_ROWS 7

typedef struct {
    uint8_t g[SCENE_TASK_LEN];
    int n, x16;     // glyph count, pen start (1/16 px)
    int base;       // text baseline
    float cy, dot_x, dot_r;
    uint16_t dot_c; // native RGB565
    bool hollow;
    int a256;       // row opacity
} pk_row_t;

static pk_row_t s_pk[PK_ROWS];
static int s_npk;

static void picker_row(uint16_t *o, int y, int hint)
{
    memset(o, 0, LCD_H_RES * sizeof(uint16_t));
    for (int k = 0; k < s_npk; k++) {
        const pk_row_t *r = &s_pk[k];
        const float dy = y + 0.5f - r->cy;
        if (dy > -r->dot_r - 2 && dy < r->dot_r + 2) {
            const int x0 = ifloor(r->dot_x - r->dot_r - 2), x1 = ifloor(r->dot_x + r->dot_r + 2);
            for (int x = imax(0, x0); x <= imin(LCD_H_RES - 1, x1); x++) {
                const float dx = x + 0.5f - r->dot_x, d = sqrtf(dx * dx + dy * dy);
                const float cov = r->hollow ? clamp01(1.25f - absf(d - r->dot_r)) : clamp01(r->dot_r + 0.5f - d);
                if (cov > 0) o[x] = mix565(o[x], r->dot_c, (int)(cov * r->a256));
            }
        }
        int pen16 = r->x16;
        for (int i = 0; i < r->n; i++) {
            const glyph_t *gl = &TASK_FONT[r->g[i]];
            const int gy = y - (r->base + gl->m.oy);
            if (gl->m.w && gy >= 0 && gy < gl->m.h) {
                const int gx0 = ((pen16 + 8) >> 4) + gl->m.ox;
                const uint8_t *src = gl->m.data + gy * gl->m.w;
                for (int j = 0; j < gl->m.w; j++) {
                    const int X = gx0 + j;
                    if (src[j] && (unsigned)X < LCD_H_RES) o[X] = mix565(o[X], 0xFFFF, (src[j] * r->a256) >> 8);
                }
            }
            pen16 += gl->adv16;
        }
    }
    if (hint >= 0 && hint < STR_COUNT) {
        const mask_t *m = &STRINGS[hint];
        const int gy = y - (316 + m->oy);
        if (gy >= 0 && gy < m->h) {
            const uint16_t gray = rgb_to_565(0x8E8E94);
            const int gx0 = (int)CX + m->ox;
            const uint8_t *src = m->data + gy * m->w;
            for (int j = 0; j < m->w; j++) {
                if (src[j]) o[gx0 + j] = mix565(o[gx0 + j], gray, src[j] + (src[j] >> 7));
            }
        }
    }
    for (int x = 0; x < LCD_H_RES; x++) o[x] = swap16(o[x]);
}

static void picker_setup(const picker_view_t *v)
{
    s_npk = 0;
    for (int i = 0; i < v->count && s_npk < PK_ROWS; i++) {
        const float a = 1.0f - 0.36f * absf(i - v->pos);
        if (a <= 0.01f) continue;
        pk_row_t *r = &s_pk[s_npk++];
        int w16;
        r->n = layout_text(v->names[i], TASK_FONT, r->g, PK_MAX_W * 16, &w16);
        const float left = CX - (20.0f + w16 / 16.0f) / 2;
        r->x16 = (int)((left + 20) * 16);
        r->dot_x = left + 6;
        r->cy = CY + (i - v->pos) * PK_ROW - 0.5f;
        r->base = ifloor(r->cy + 0.5f) + 6;
        r->dot_r = i == v->selected ? 6.0f : 4.5f;
        r->hollow = v->hollow[i];
        r->dot_c = rgb_to_565(v->hollow[i] ? 0x8E8E94 : v->colors[i]);
        r->a256 = (int)(a * 256);
    }
}

void render_picker(const picker_view_t *v)
{
    const int64_t t0 = esp_timer_get_time();
    picker_setup(v);
    for (int y0 = 0; y0 < LCD_V_RES; y0 += DISPLAY_BAND_ROWS) {
        uint16_t *out = display_band_buffer();
        for (int yy = 0; yy < DISPLAY_BAND_ROWS; yy++) {
            uint16_t *o = out + yy * LCD_H_RES;
            const int Y = y0 + yy;
            picker_row(o, s_rot180 ? LCD_V_RES - 1 - Y : Y, v->hint);
            if (s_rot180) reverse_row(o);
        }
        display_flush_band(y0, DISPLAY_BAND_ROWS);
    }
    s_has_last = false; // back on the dial, everything is redrawn
    s_stats.frames++;
    s_stats.bands += LCD_V_RES / DISPLAY_BAND_ROWS;
    s_stats.total_us += esp_timer_get_time() - t0;
}

// ---- Debug capture -------------------------------------------------------------

void render_capture(const scene_t *s, const picker_view_t *v, uint16_t *out)
{
    if (v) {
        picker_setup(v);
        for (int y = 0; y < LCD_V_RES; y++) picker_row(out + y * LCD_H_RES, y, v->hint);
    } else {
        frame_setup(s);
        for (int y = 0; y < LCD_V_RES; y++) render_row(out + y * LCD_H_RES, y);
    }
    s_has_last = false; // the layers were rebuilt; redraw everything next frame
}
