// Software renderer for the Knob Timer face (same layout as the HTML preview).
//
// Layers, bottom to top:
//   static background: face, project pill, rim band and ticks  ->  pulse glow  ->
//   text (task, time, status, mic, waveform)  ->  Wi-Fi glyph  ->  hold ring
//
// The rim: two static backgrounds are built whenever the profile, mode, project
// or Wi-Fi glyph changes: one with the rim "off" (Pomodoro: black band with a
// faint ring; Tracking: the bare face) and one with the band "on" in the rim
// color, ticks turned into dark notches. Each frame picks between them per
// pixel by angle (the Pomodoro arc), with whole ticks switching at once, as in
// the preview.
//
// One text layer holds coverage; its color depends on the row: the task rows
// use the task color, everything below uses the live color (time, status, mic).
//
// Performance notes (ESP32-S3):
// - libm calls (sqrtf, floorf, ...) are not inlined on Xtensa and float division
//   is in software, so per-pixel loops avoid them.
// - Only bands touched by a change are redrawn (see dirty_bands).
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
#include "esp_timer.h"

static const char *TAG = "render";

#define CX 180.0f
#define CY 180.0f
#define W LCD_H_RES
#define BAND_R0 155.0f // rim band: from here to the edge (stroke 25 at r 167.5)
#define BACK_R0 153.0f // Pomodoro: black backing from here (stroke 28 at r 167), a hairline inside the band
#define RIM_IN 152.0f  // pixels closer to the center than this are never part of the rim
#define BASE_A 0.16f   // Pomodoro: the full ring under the arc, at 16% of the rim color
#define RING_R 128.0f  // hold ring
#define TIME_BASELINE 212
#define TASK_BASELINE 123
#define STATUS_BASELINE 238
#define PILL_BASELINE 69
#define PILL_TEXT_MAX 120 // px; a longer project name ends in an ellipsis
#define PILL_PAD 32       // pill width = text + this
#define MIC_CY ((float)RENDER_MIC_CY)
#define WAVE_CY 238.0f
#define WIFI_CY 347 // Wi-Fi glyph anchor (its arcs' center is at 352.5, the middle of the band)
#define TASK_X0 (180 - TASK_MAX_W / 2)
#define TASK_FADE 20 // px: a scrolling task fades out at the window edges

// Text layer region: task, time (with pulse swell and glow), status, waveform, mic.
#define TX0 20
#define TY0 88
#define TW 320
#define TH 232
#define LIVE_ROW 140 // text rows from here down use the live color; above, the task color

typedef struct {
    uint8_t *px;            // TW x TH coverage
    int16_t lo[TH], hi[TH]; // per-row written x range (layer coords), lo > hi = empty
} layer_t;

// What the static backgrounds were built from.
typedef struct {
    int theme;
    bool pomo, wifi_rim, has_project;
    uint32_t rim_rgb, project_rgb;
    char project[PROJECT_NAME_LEN];
} bg_params_t;

static uint16_t *s_bg_off, *s_bg_on; // byte-swapped RGB565, 360 x 360 each
static uint8_t *s_tick;              // per pixel: 0, or 1 + index of the tick (0 = 12 o'clock) it belongs to
static bg_params_t s_bgp;
static bool s_bg_valid;
static layer_t s_text, s_glow;
static bool s_rot180;
static scene_t s_last;
static bool s_has_last;
static render_stats_t s_stats;

// Per-frame constants
static struct {
    bool full, empty, convex, glow_on, ring_on;
    float bx, by, ring_end;
    bool tick_on[12];
    int wifi_a256;
    bool wifi_on;
    uint16_t wifi_c;                  // glyph color when off (a tick color)
    uint16_t live, task_c, face, sub; // native RGB565
} F;

static inline uint16_t swap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
static inline float minf(float a, float b) { return a < b ? a : b; }
static inline float maxf(float a, float b) { return a > b ? a : b; }
static inline float absf(float v) { return v < 0 ? -v : v; }
static inline int ifloor(float v) { return (int)(v + 1024.0f) - 1024; } // floor for |v| < 1024, no libcall
static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

// Per-channel blend of native RGB565 colors with a 0..256 alpha.
static inline uint16_t mix565(uint16_t a, uint16_t b, int t)
{
    int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31;
    const int br = b >> 11, bgn = (b >> 5) & 63, bb = b & 31;
    ar += ((br - ar) * t + 128) >> 8;
    ag += ((bgn - ag) * t + 128) >> 8;
    ab += ((bb - ab) * t + 128) >> 8;
    return (uint16_t)(ar << 11 | ag << 5 | ab);
}

static inline uint16_t mixf(uint16_t a, uint16_t b, float t) { return mix565(a, b, (int)(t * 256 + 0.5f)); }

void render_take_stats(render_stats_t *out)
{
    *out = s_stats;
    memset(&s_stats, 0, sizeof s_stats);
}

// ---- Text layout ------------------------------------------------------------------

// Decode UTF-8 into font indices. Accented Latin letters lose their accents,
// typographic quotes and dashes become ASCII, anything else (emoji...) is dropped.
static int text_glyphs(const char *s, uint8_t *out, int cap)
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
            out[n++] = FONT_ELLIPSIS;
            continue;
        }
        if (ch >= 0) out[n++] = (uint8_t)(ch - FONT_FIRST);
    }
    return n;
}

// Glyphs for `text` in `font`, cut with an ellipsis to fit max16 (1/16 px; 0 = no limit).
// Returns the count and the width.
static int layout_text(const char *text, const glyph_t *font, uint8_t *g, int max16, int *w16_out)
{
    int n = text_glyphs(text, g, SCENE_TASK_LEN - 1);
    int w16 = 0;
    for (int i = 0; i < n; i++) w16 += font[g[i]].adv16;
    if (max16 && w16 > max16) { // cut at a glyph boundary, drop trailing spaces, add an ellipsis
        const int ell = font[FONT_ELLIPSIS].adv16;
        while (n > 0 && (w16 + ell > max16 || g[n - 1] == ' ' - FONT_FIRST)) w16 -= font[g[--n]].adv16;
        g[n++] = FONT_ELLIPSIS;
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

int render_pill_width(const char *name)
{
    uint8_t g[SCENE_TASK_LEN];
    int w16;
    layout_text(name, PILL_FONT, g, PILL_TEXT_MAX * 16, &w16);
    return (w16 + 15) / 16 + PILL_PAD;
}

static inline int mask_px(const mask_t *m, int x, int y)
{
    return ((unsigned)x >= m->w || (unsigned)y >= m->h) ? 0 : m->data[y * m->w + x];
}

// ---- Static backgrounds -------------------------------------------------------------

// The project pill (stadium, y 50..78) with its name, drawn into the "off" background.
// Returns the pill's x range.
static void build_pill(const bg_params_t *bp, int *px0, int *px1)
{
    const theme_t *th = &THEMES[bp->theme];
    const char *name = bp->has_project ? bp->project : "No project";
    uint8_t g[SCENE_TASK_LEN];
    int w16;
    const int n = layout_text(name, PILL_FONT, g, PILL_TEXT_MAX * 16, &w16);
    const float pw = w16 / 16.0f + PILL_PAD, r = RENDER_PILL_H / 2.0f;
    const float x0 = CX - pw / 2, x1 = CX + pw / 2, ymid = RENDER_PILL_Y0 + r;
    const uint16_t fill = rgb_to_565(bp->project_rgb), sub = rgb_to_565(th->sub);
    *px0 = ifloor(x0) - 1;
    *px1 = ifloor(x1) + 2;
    for (int y = RENDER_PILL_Y0 - 1; y <= RENDER_PILL_Y0 + RENDER_PILL_H + 1; y++) {
        for (int x = *px0; x < *px1; x++) {
            const float fx = x + 0.5f, fy = y + 0.5f;
            const float cx = fx < x0 + r ? x0 + r : (fx > x1 - r ? x1 - r : fx);
            const float d = sqrtf((fx - cx) * (fx - cx) + (fy - ymid) * (fy - ymid)) - r; // signed distance
            uint16_t c = swap16(s_bg_off[y * W + x]);
            if (bp->has_project) c = mixf(c, fill, clamp01(0.5f - d));
            else c = mixf(c, sub, clamp01(1.25f - absf(d))); // 1.5 px outline on the edge
            s_bg_off[y * W + x] = swap16(c);
        }
    }
    const uint16_t ink = bp->has_project ? rgb_to_565(readable_on(bp->project_rgb)) : sub;
    int pen16 = (int)(CX * 16) - w16 / 2;
    for (int i = 0; i < n; i++) {
        const glyph_t *gl = &PILL_FONT[g[i]];
        const int lx = ((pen16 + 8) >> 4) + gl->m.ox, ty = PILL_BASELINE + gl->m.oy;
        for (int my = 0; my < gl->m.h; my++) {
            for (int mx = 0; mx < gl->m.w; mx++) {
                const int v = gl->m.data[my * gl->m.w + mx];
                if (!v) continue;
                uint16_t *p = &s_bg_off[(ty + my) * W + lx + mx];
                *p = swap16(mix565(swap16(*p), ink, v + (v >> 7)));
            }
        }
        pen16 += gl->adv16;
    }
}

static void build_bg(const bg_params_t *bp)
{
    const int64_t t0 = esp_timer_get_time();
    const theme_t *th = &THEMES[bp->theme];
    const theme_t *rt = bp->pomo ? &THEMES[THEME_MONO] : th; // Pomodoro's rim looks the same in both profiles
    const uint16_t face = rgb_to_565(th->face), rim = rgb_to_565(bp->rim_rgb);
    const uint16_t black = 0, base = mixf(0, rim, BASE_A);
    const uint16_t tick = rgb_to_565(rt->tick), major = rgb_to_565(rt->major);
    float ux[12], uy[12];
    for (int k = 0; k < 12; k++) {
        ux[k] = sinf(k * (float)M_PI / 6);
        uy[k] = -cosf(k * (float)M_PI / 6);
    }
    const uint16_t face_sw = swap16(face);
    for (int y = 0; y < LCD_V_RES; y++) {
        const float dy = y + 0.5f - CY;
        for (int x = 0; x < W; x++) {
            const int i = y * W + x;
            const float dx = x + 0.5f - CX;
            const float r2 = dx * dx + dy * dy;
            s_tick[i] = 0;
            if (r2 < RIM_IN * RIM_IN) {
                s_bg_off[i] = s_bg_on[i] = face_sw;
                continue;
            }
            const float r = sqrtf(r2);
            uint16_t off = face, on = face;
            if (bp->pomo) { // black backing under the band
                const float back = clamp01(r - (BACK_R0 - 0.5f));
                off = on = mixf(face, black, back);
            }
            const float band = clamp01(r - (BAND_R0 - 0.5f));
            if (bp->pomo) off = mixf(off, base, band);
            on = mixf(on, rim, band);
            if (r > 154.0f && r < 172.0f) {
                float phi = atan2f(dx, -dy);
                if (phi < 0) phi += 2 * (float)M_PI;
                const int k = (int)lroundf(phi * (6 / (float)M_PI)) % 12;
                if (!(bp->wifi_rim && k == 6)) { // the Wi-Fi glyph replaces the 6 o'clock tick
                    float t = dx * ux[k] + dy * uy[k];
                    t = t < 158 ? 158 : (t > 168 ? 168 : t);
                    const float px = dx - t * ux[k], py = dy - t * uy[k];
                    const bool is_major = k % 3 == 0;
                    const float cov = clamp01((is_major ? 1.5f : 1.0f) + 0.5f - sqrtf(px * px + py * py));
                    if (cov > 0) {
                        const float notch = (is_major ? 0.62f : 0.42f) * cov; // rgba(0,0,0,.42 / .62) on the band
                        // The 12 o'clock tick straddles the arc's start: while the arc is partial, its left
                        // half is a notch over the uncovered band (the renderer mirrors the right half for a full ring).
                        on = mixf(k == 0 && x < 180 ? off : on, black, notch);
                        off = mixf(off, is_major ? major : tick, cov);
                        s_tick[i] = (uint8_t)(k + 1);
                    }
                }
            }
            s_bg_off[i] = swap16(off);
            s_bg_on[i] = swap16(on);
        }
    }
    int px0, px1;
    build_pill(bp, &px0, &px1);
    for (int y = RENDER_PILL_Y0 - 1; y <= RENDER_PILL_Y0 + RENDER_PILL_H + 1; y++) { // (inside the rim: same in both)
        memcpy(s_bg_on + y * W + px0, s_bg_off + y * W + px0, (px1 - px0) * sizeof(uint16_t));
    }
    ESP_LOGI(TAG, "background built in %lld ms", (esp_timer_get_time() - t0) / 1000);
}

static void scene_bg_params(const scene_t *s, bg_params_t *bp)
{
    memset(bp, 0, sizeof *bp);
    bp->theme = s->theme;
    bp->pomo = s->pomo;
    bp->wifi_rim = s->wifi_rim;
    bp->has_project = s->has_project;
    bp->rim_rgb = s->rim_rgb;
    bp->project_rgb = s->has_project ? s->project_rgb : 0;
    if (s->has_project) strlcpy(bp->project, s->project, sizeof bp->project);
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

// Draw a mask anchored at (ax, ay), scaled by sc around (scx, scy); overlapping
// coverage keeps the max. sc == 1 is a straight copy; otherwise bilinear.
static void blit(layer_t *L, const mask_t *m, float ax, float ay, float sc, float scx, float scy, float alpha)
{
    if (alpha <= 0.004f || !m->w) return;
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

// The time, centered: "MM:SS" at 88 px or "HH:MM:SS" at 62 px, tabular figures.
static void draw_time(layer_t *L, const glyph_t *font, const char *t, float sc, float alpha)
{
    int w16 = 0;
    for (const char *p = t; *p; p++) w16 += font[*p == ':' ? TIME_COLON : *p - '0'].adv16;
    int pen16 = (int)(CX * 16) - w16 / 2;
    for (const char *p = t; *p; p++) {
        const glyph_t *gl = &font[*p == ':' ? TIME_COLON : *p - '0'];
        blit(L, &gl->m, pen16 / 16.0f, TIME_BASELINE, sc, CX, CY, alpha);
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

// The task: centered if it fits; otherwise it scrolls through the window in a loop, with a gap.
static void draw_task(layer_t *L, const char *text, float alpha, int scroll)
{
    uint8_t g[SCENE_TASK_LEN];
    int w16;
    const int n = layout_text(text, TASK_FONT, g, 0, &w16);
    if (scroll < 0) {
        int pen16 = (int)(CX * 16) - w16 / 2;
        for (int i = 0; i < n; i++) {
            const glyph_t *gl = &TASK_FONT[g[i]];
            blit(L, &gl->m, (float)((pen16 + 8) >> 4), TASK_BASELINE, 1.0f, CX, CY, alpha);
            pen16 += gl->adv16;
        }
        return;
    }
    const int wx0 = TASK_X0, wx1 = TASK_X0 + TASK_MAX_W;
    const int a256 = (int)(alpha * 256 + 0.5f);
    for (int copy = 0; copy < 2; copy++) {
        int pen16 = (wx0 - scroll + copy * ((w16 + 15) / 16 + TASK_SCROLL_GAP)) * 16;
        for (int i = 0; i < n; i++) {
            const glyph_t *gl = &TASK_FONT[g[i]];
            if (gl->m.w) blit_window(L, &gl->m, (pen16 + 8) >> 4, TASK_BASELINE, a256, wx0, wx1);
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
    const bool small = strlen(s->time) > 5;
    draw_time(&s_text, small ? TIME_SMALL_FONT : TIME_FONT, s->time, s->time_scale, s->time_alpha);
    if (s->task[0]) draw_task(&s_text, s->task, s->task_alpha, s->task_scroll);
    if (s->status >= 0 && s->status < STR_COUNT) {
        blit(&s_text, &STRINGS[s->status], CX, STATUS_BASELINE, 1.0f, CX, CY, s->status_alpha);
    }
    if (s->wave_on) { // 21 rounded bars, 4 px wide, 7 px apart
        for (int i = 0; i < 21; i++) bar(&s_text, CX + (i - 10) * 7, WAVE_CY, s->wave[i]);
    }
    if (s->mic_ring_alpha > 0.004f) blit(&s_text, &MIC_RING, CX, MIC_CY, 1.0f, CX, CY, s->mic_ring_alpha);
    if (s->mic_alpha > 0.004f) blit(&s_text, &MIC_ICON, CX, MIC_CY, 1.0f, CX, CY, s->mic_alpha);
    // The glow is blurred, so it is drawn at rest size; only its strength pulses. (Only MM:SS pulses.)
    layer_clear(&s_glow);
    if (s->glow > 0.004f && !small) draw_time(&s_glow, TIME_GLOW, s->time, 1.0f, s->glow * s->time_alpha);
}

// ---- Dirty rows -------------------------------------------------------------

// Dirty rows are tracked per 24-row band (a bit per band, in scene rows), so changes at the
// top, middle and bottom of the screen don't merge into one full-screen redraw.
#define NBANDS (LCD_V_RES / DISPLAY_BAND_ROWS)
#define ALL_BANDS ((1u << NBANDS) - 1)

static void add_rows(uint32_t *mask, int lo, int hi) // rows [lo, hi)
{
    lo = imax(0, lo);
    hi = imin(LCD_V_RES, hi);
    for (int b = lo / DISPLAY_BAND_ROWS; hi > lo && b <= (hi - 1) / DISPLAY_BAND_ROWS; b++) *mask |= 1u << b;
}

// Rows covered by the arc between angles a1..a2 (degrees clockwise from 12)
// between radii r0 and r1 (r0 = 0: including the center).
static void sector_rows(float a1, float a2, float r0, float r1, uint32_t *mask)
{
    if (a1 > a2) {
        const float t = a1;
        a1 = a2;
        a2 = t;
    }
    float ymin = 1e9f, ymax = -1e9f;
    const float as[2] = {a1, a2}, rs[2] = {r0, r1};
    for (int i = 0; i < 2; i++) {
        const float c = cosf(as[i] * (float)M_PI / 180);
        for (int j = 0; j < 2; j++) {
            const float y = CY - rs[j] * c;
            ymin = minf(ymin, y);
            ymax = maxf(ymax, y);
        }
    }
    if (a1 <= 0.0f || a2 >= 360.0f) ymin = CY - r1; // passes 12 o'clock
    if (a1 < 180.0f && a2 > 180.0f) ymax = CY + r1; // passes 6 o'clock
    add_rows(mask, ifloor(ymin) - 3, ifloor(ymax) + 4);
}

static uint32_t dirty_bands(const scene_t *s)
{
    const scene_t *p = &s_last;
    uint32_t m = 0;
    if (!s_has_last || s->theme != p->theme || s->pomo != p->pomo || s->rim_rgb != p->rim_rgb ||
        s->has_project != p->has_project || s->project_rgb != p->project_rgb || strcmp(s->project, p->project) ||
        s->wifi_rim != p->wifi_rim) {
        return ALL_BANDS;
    }
    if (s->rim_frac != p->rim_frac) {
        sector_rows(minf(s->rim_frac, p->rim_frac) * 360 - 2, maxf(s->rim_frac, p->rim_frac) * 360 + 2, RIM_IN - 2,
                    182, &m);
        const bool on_now = s->rim_frac >= 0.9995f || s->rim_frac > 0.5f;
        const bool on_before = p->rim_frac >= 0.9995f || p->rim_frac > 0.5f;
        if (on_now != on_before) add_rows(&m, WIFI_CY - 13, WIFI_CY + 13);
    }
    if (s->hold_progress != p->hold_progress) {
        sector_rows(p->hold_progress * 360, s->hold_progress * 360, 0, RING_R + 3, &m);
    }
    if (rgb_to_565(s->live_rgb) != rgb_to_565(p->live_rgb)) add_rows(&m, 118, TY0 + TH);
    if (strcmp(s->time, p->time) || s->time_alpha != p->time_alpha || s->time_scale != p->time_scale ||
        s->glow != p->glow) {
        add_rows(&m, 118, 242);
    }
    if (strcmp(s->task, p->task) || s->task_untitled != p->task_untitled || s->task_alpha != p->task_alpha ||
        s->task_scroll != p->task_scroll) {
        add_rows(&m, TY0, LIVE_ROW);
    }
    if (s->status != p->status || s->status_alpha != p->status_alpha) add_rows(&m, 224, 248);
    if (s->wave_on != p->wave_on || memcmp(s->wave, p->wave, sizeof s->wave)) add_rows(&m, 224, 252);
    if (s->mic_alpha != p->mic_alpha || s->mic_ring_alpha != p->mic_ring_alpha) add_rows(&m, 244, TY0 + TH);
    if (s->wifi_alpha != p->wifi_alpha) add_rows(&m, WIFI_CY - 13, WIFI_CY + 13);
    return m;
}

// ---- Row passes -------------------------------------------------------------

// Rim pixels [xa, xb) of row y: the "on" background inside the arc, "off" outside.
static void rim_span(uint16_t *o, const uint16_t *off, const uint16_t *on, int y, int xa, int xb)
{
    if (xb <= xa) return;
    if (F.empty) {
        memcpy(o + xa, off + xa, (xb - xa) * sizeof(uint16_t));
        return;
    }
    const uint8_t *tk = s_tick + y * W;
    if (F.full) {
        memcpy(o + xa, on + xa, (xb - xa) * sizeof(uint16_t));
        if (y < 30) { // the 12 o'clock tick's left half: a notch over the band, like its right half
            for (int x = imax(xa, 170); x < imin(xb, 180); x++) {
                if (tk[x] == 1) o[x] = on[W - 1 - x];
            }
        }
        return;
    }
    const float dy = y + 0.5f - CY, bx = F.bx, by = F.by;
    for (int x = xa; x < xb; x++) {
        const int k = tk[x];
        if (k) { // ticks switch as a whole
            o[x] = F.tick_on[k - 1] ? on[x] : off[x];
            continue;
        }
        const float dx = x + 0.5f - CX;
        float cov;
        const float raw = dx * by - dy * bx; // signed distance to the arc's end edge, > 0 before it
        if (F.convex) {
            // Right half only. Near the end edge, anti-alias; more than 90° away from it (behind the
            // center), the sign alone decides: before the end (raw > 0) is covered.
            if (dx < 0) cov = 0;
            else if (dx * bx + dy * by > 0) cov = clamp01(raw + 0.5f);
            else cov = raw > 0 ? 1.0f : 0.0f;
        } else {
            cov = dx >= 0 ? 1.0f : clamp01(raw + 0.5f);
        }
        if (cov <= 0) o[x] = off[x];
        else if (cov >= 1) o[x] = on[x];
        else o[x] = swap16(mixf(swap16(off[x]), swap16(on[x]), cov));
    }
}

// Pixels under text and glow: glow, then the text in its row's color.
static void composite_text(uint16_t *o, const uint16_t *bg, int x0, int x1, const uint8_t *trow, const uint8_t *grow,
                           uint16_t tc)
{
    const uint16_t live = F.live;
    for (int x = x0; x < x1; x++) {
        const int t = trow[x], g = grow ? grow[x] : 0;
        if ((t | g) == 0) {
            o[x] = bg[x];
            continue;
        }
        uint16_t px = swap16(bg[x]);
        if (g) px = mix565(px, live, g + (g >> 7));
        if (t) px = mix565(px, tc, t + (t >> 7));
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
        for (int x = imax(0, spans[k][0]); x < imin(W, spans[k][1]); x++) {
            const float dx = x + 0.5f - CX;
            float phi = atan2f(dx, -dy);
            if (phi < 0) phi += 2 * (float)M_PI;
            if (phi > F.ring_end) continue;
            const float rg = clamp01(2.0f - absf(sqrtf(dx * dx + dy * dy) - RING_R)) * 0.8f;
            if (rg > 0) o[x] = swap16(mix565(swap16(o[x]), F.sub, (int)(rg * 256)));
        }
        if (ih == 0) break; // single span through the top/bottom of the ring
    }
}

// The Wi-Fi glyph in the rim at 6 o'clock: a tick color, or a dark notch when the band covers it.
static void composite_wifi(uint16_t *o, int y)
{
    const int my = y - (WIFI_CY + WIFI_ICON.oy);
    for (int mx = 0; mx < WIFI_ICON.w; mx++) {
        int cov = mask_px(&WIFI_ICON, mx, my) * F.wifi_a256 >> 8;
        if (!cov) continue;
        const int x = (int)CX + WIFI_ICON.ox + mx;
        if (F.wifi_on) o[x] = swap16(mix565(swap16(o[x]), 0, (cov * 159) >> 8)); // rgba(0,0,0,.62)
        else o[x] = swap16(mix565(swap16(o[x]), F.wifi_c, cov + (cov >> 7)));
    }
}

static void render_row(uint16_t *o, int y)
{
    const uint16_t *off = s_bg_off + y * W, *on = s_bg_on + y * W;
    const float dy = y + 0.5f - CY;
    const float in2 = RIM_IN * RIM_IN - dy * dy;
    int xi0 = 180, xi1 = 180; // pixels in [xi0, xi1) are inside the rim
    if (in2 > 0) {
        const float ih = sqrtf(in2);
        xi0 = imin(180, (int)(CX - ih) + 1);
        xi1 = imax(180, (int)(CX + ih) - 1);
    }
    rim_span(o, off, on, y, 0, xi0);
    rim_span(o, off, on, y, xi1, W);

    int t0 = xi1, t1 = xi0; // text/glow x range in this row
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
        t0 = imax(t0, xi0);
        t1 = imin(t1, xi1);
    }
    if (t1 > t0) {
        memcpy(o + xi0, off + xi0, (t0 - xi0) * sizeof(uint16_t));
        composite_text(o, off, t0, t1, trow, grow, y < LIVE_ROW ? F.task_c : F.live);
        memcpy(o + t1, off + t1, (xi1 - t1) * sizeof(uint16_t));
    } else if (xi1 > xi0) {
        memcpy(o + xi0, off + xi0, (xi1 - xi0) * sizeof(uint16_t));
    }
    if (F.wifi_a256 && y >= WIFI_CY - 12 && y < WIFI_CY + 12) composite_wifi(o, y);
    if (F.ring_on) composite_ring(o, y);
}

// ---- Frame --------------------------------------------------------------------

void render_init(void)
{
    // PSRAM: the per-row reads are sequential and cache well, and internal RAM is
    // needed by Wi-Fi and TLS (uploads slowed down badly with less than ~20 KB free).
    s_bg_off = heap_caps_malloc(W * LCD_V_RES * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    s_bg_on = heap_caps_malloc(W * LCD_V_RES * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    s_tick = heap_caps_malloc(W * LCD_V_RES, MALLOC_CAP_SPIRAM);
    s_text.px = heap_caps_calloc(1, TW * TH, MALLOC_CAP_SPIRAM);
    s_glow.px = heap_caps_calloc(1, TW * TH, MALLOC_CAP_SPIRAM);
    assert(s_bg_off && s_bg_on && s_tick && s_text.px && s_glow.px);
    for (int r = 0; r < TH; r++) {
        s_text.lo[r] = s_glow.lo[r] = TW;
        s_text.hi[r] = s_glow.hi[r] = -1;
    }
    ESP_LOGI(TAG, "layers ready (in PSRAM)");
}

void render_invalidate(void) { s_has_last = false; }

void render_set_rotation(int deg)
{
    s_rot180 = (deg % 360) == 180;
    s_has_last = false;
}

static void reverse_row(uint16_t *o)
{
    for (int i = 0, j = W - 1; i < j; i++, j--) {
        const uint16_t t = o[i];
        o[i] = o[j];
        o[j] = t;
    }
}

// Layers and per-frame constants for a scene.
static void frame_setup(const scene_t *s)
{
    bg_params_t bp;
    scene_bg_params(s, &bp);
    if (!s_bg_valid || memcmp(&bp, &s_bgp, sizeof bp) != 0) {
        build_bg(&bp);
        s_bgp = bp;
        s_bg_valid = true;
    }
    build_text(s);
    const theme_t *th = &THEMES[s->theme];
    F.live = rgb_to_565(s->live_rgb);
    F.task_c = rgb_to_565(s->task_untitled ? th->sub : th->ink);
    F.face = rgb_to_565(th->face);
    F.sub = rgb_to_565(th->sub);
    F.glow_on = s->glow > 0.004f;
    F.full = s->rim_frac >= 0.9995f;
    F.empty = s->rim_frac <= 0.0005f;
    F.convex = s->rim_frac <= 0.5f;
    const float th_rad = s->rim_frac * 2 * (float)M_PI;
    F.bx = sinf(th_rad);
    F.by = -cosf(th_rad);
    const float deg = s->rim_frac * 360;
    for (int k = 0; k < 12; k++) F.tick_on[k] = F.full || (k == 0 ? deg > 0.5f : k * 30 < deg - 0.5f);
    F.wifi_on = F.full || deg > 180;
    F.wifi_a256 = s->wifi_rim ? (int)(s->wifi_alpha * 256) : 0;
    F.wifi_c = rgb_to_565((s->pomo ? &THEMES[THEME_MONO] : th)->major);
    F.ring_end = s->hold_progress * 2 * (float)M_PI;
    F.ring_on = F.ring_end > 0;
}

void render_frame(const scene_t *s)
{
    const int64_t t0 = esp_timer_get_time();
    const uint32_t dirty = dirty_bands(s);
    frame_setup(s);
    s_stats.text_us += esp_timer_get_time() - t0;
    s_stats.frames++;

    // Bands are in screen rows; with a 180° rotation, screen row Y shows scene row 359 - Y mirrored.
    for (int y0 = 0; y0 < LCD_V_RES; y0 += DISPLAY_BAND_ROWS) {
        const int s0 = s_rot180 ? LCD_V_RES - (y0 + DISPLAY_BAND_ROWS) : y0;
        if (!(dirty & (1u << (s0 / DISPLAY_BAND_ROWS)))) continue; // band unchanged
        const int64_t w0 = esp_timer_get_time();
        uint16_t *out = display_band_buffer();
        s_stats.wait_us += esp_timer_get_time() - w0;
        for (int yy = 0; yy < DISPLAY_BAND_ROWS; yy++) {
            uint16_t *o = out + yy * W;
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

// ---- Lists ----------------------------------------------------------------------
// A list on the face color: each row is a color dot and a name, centered together.
// The selected row sits in the middle; the others fade with distance.

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
static uint16_t s_pk_face, s_pk_ink, s_pk_sub;

static void picker_row(uint16_t *o, int y, int hint)
{
    for (int x = 0; x < W; x++) o[x] = s_pk_face;
    for (int k = 0; k < s_npk; k++) {
        const pk_row_t *r = &s_pk[k];
        const float dy = y + 0.5f - r->cy;
        if (dy > -r->dot_r - 2 && dy < r->dot_r + 2) {
            const int x0 = ifloor(r->dot_x - r->dot_r - 2), x1 = ifloor(r->dot_x + r->dot_r + 2);
            for (int x = imax(0, x0); x <= imin(W - 1, x1); x++) {
                const float dx = x + 0.5f - r->dot_x, d = sqrtf(dx * dx + dy * dy);
                const float cov = r->hollow ? clamp01(1.25f - absf(d - r->dot_r)) : clamp01(r->dot_r + 0.5f - d);
                if (cov > 0) o[x] = mix565(o[x], r->dot_c, (int)(cov * r->a256));
            }
        }
        int pen16 = r->x16;
        for (int i = 0; i < r->n; i++) {
            const glyph_t *gl = &LIST_FONT[r->g[i]];
            const int gy = y - (r->base + gl->m.oy);
            if (gl->m.w && gy >= 0 && gy < gl->m.h) {
                const int gx0 = ((pen16 + 8) >> 4) + gl->m.ox;
                const uint8_t *src = gl->m.data + gy * gl->m.w;
                for (int j = 0; j < gl->m.w; j++) {
                    const int X = gx0 + j;
                    if (src[j] && (unsigned)X < W) o[X] = mix565(o[X], s_pk_ink, (src[j] * r->a256) >> 8);
                }
            }
            pen16 += gl->adv16;
        }
    }
    if (hint >= 0 && hint < STR_COUNT) {
        const mask_t *m = &STRINGS[hint];
        const int gy = y - (316 + m->oy);
        if (gy >= 0 && gy < m->h) {
            const int gx0 = (int)CX + m->ox;
            const uint8_t *src = m->data + gy * m->w;
            for (int j = 0; j < m->w; j++) {
                if (src[j]) o[gx0 + j] = mix565(o[gx0 + j], s_pk_sub, src[j] + (src[j] >> 7));
            }
        }
    }
    for (int x = 0; x < W; x++) o[x] = swap16(o[x]);
}

static void picker_setup(const picker_view_t *v)
{
    const theme_t *th = &THEMES[v->theme];
    s_pk_face = rgb_to_565(th->face);
    s_pk_ink = rgb_to_565(th->ink);
    s_pk_sub = rgb_to_565(th->sub);
    s_npk = 0;
    for (int i = 0; i < v->count && s_npk < PK_ROWS; i++) {
        const float a = 1.0f - 0.36f * absf(i - v->pos);
        if (a <= 0.01f) continue;
        pk_row_t *r = &s_pk[s_npk++];
        int w16;
        r->n = layout_text(v->names[i], LIST_FONT, r->g, PK_MAX_W * 16, &w16);
        const float left = CX - (20.0f + w16 / 16.0f) / 2;
        r->x16 = (int)((left + 20) * 16);
        r->dot_x = left + 6;
        r->cy = CY + (i - v->pos) * PK_ROW - 0.5f;
        r->base = ifloor(r->cy + 0.5f) + 6;
        r->dot_r = i == v->selected ? 6.0f : 4.5f;
        r->hollow = v->hollow[i];
        r->dot_c = v->hollow[i] ? s_pk_sub : rgb_to_565(v->colors[i]);
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
            uint16_t *o = out + yy * W;
            const int Y = y0 + yy;
            picker_row(o, s_rot180 ? LCD_V_RES - 1 - Y : Y, v->hint);
            if (s_rot180) reverse_row(o);
        }
        display_flush_band(y0, DISPLAY_BAND_ROWS);
    }
    s_has_last = false; // back on the face, everything is redrawn
    s_stats.frames++;
    s_stats.bands += LCD_V_RES / DISPLAY_BAND_ROWS;
    s_stats.total_us += esp_timer_get_time() - t0;
}

// ---- Debug capture -------------------------------------------------------------

void render_capture(const scene_t *s, const picker_view_t *v, uint16_t *out)
{
    if (v) {
        picker_setup(v);
        for (int y = 0; y < LCD_V_RES; y++) picker_row(out + y * W, y, v->hint);
    } else {
        frame_setup(s);
        for (int y = 0; y < LCD_V_RES; y++) render_row(out + y * W, y);
    }
    s_has_last = false; // the layers were rebuilt; redraw everything next frame
}
