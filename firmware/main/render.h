#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "project.h"

#define SCENE_TASK_LEN 96  // bytes of UTF-8, including the terminator
#define TASK_MAX_W 250     // px; a wider task scrolls (marquee) through the window x 55..305
#define TASK_SCROLL_GAP 48 // px between the end of a scrolling task and its next pass

// Layout in interface pixels (360 x 360, center 180,180), same as the preview.
#define RENDER_PILL_Y0 50 // project pill: y 50..78, centered
#define RENDER_PILL_H 28
#define RENDER_MIC_CY 282 // mic button center (x = 180)
#define RENDER_STATUS_BOTTOM 243

// Everything the screen shows.
typedef struct {
    int theme;        // THEME_MONO / THEME_INVERTED: face, text and (in Tracking) rim colors
    bool pomo;        // Pomodoro rim: black band, a faint full ring and Mono ticks in either profile
    float rim_frac;   // the rim arc, clockwise from 12 o'clock: 0..1 (1 = full ring)
    uint32_t rim_rgb; // its color (0xRRGGBB)
    bool has_project; // the pill: filled with project_rgb, or an outlined "No project"
    uint32_t project_rgb;
    char project[PROJECT_NAME_LEN];
    uint32_t live_rgb;         // time, status, mic and waveform
    char time[9];              // "MM:SS" (88 px) or "HH:MM:SS" (62 px)
    float time_alpha;          // 1, or 0.55 while paused
    float time_scale;          // last-minute pulse swell around (180, 180), 1 = rest
    float glow;                // last-minute pulse glow (0..1)
    char task[SCENE_TASK_LEN]; // the task (UTF-8), or "Untitled"
    bool task_untitled;        // drawn in the secondary color
    float task_alpha;
    int task_scroll;           // -1 = fits (centered); otherwise the marquee offset in px
    int status;                // str_id_t, or -1 for none
    float status_alpha;
    bool wave_on;              // waveform on the status line while listening
    uint8_t wave[21];          // bar heights in px
    float mic_alpha;           // mic button, 0 = hidden
    float mic_ring_alpha;
    bool wifi_rim;             // the Wi-Fi glyph replaces the 6 o'clock tick
    float wifi_alpha;          // its strength (pulses while reconnecting)
    float hold_progress;       // long-press ring, 0 = hidden
} scene_t;

// Full-screen list (past tasks or projects), scrolled with the knob.
#define PICKER_MAX 51
typedef struct {
    int theme;
    int count;
    const char *names[PICKER_MAX];
    uint32_t colors[PICKER_MAX]; // dot colors
    bool hollow[PICKER_MAX];     // drawn as an empty ring ("Untitled", "No project")
    bool action[PICKER_MAX];     // no dot, in the secondary color ("Sync from Toggl")
    int selected;
    float pos;                   // scroll position, glides toward `selected`
    int hint;                    // str_id_t shown at the bottom, -1 = none
} picker_view_t;

typedef struct {
    int frames, bands;                  // bands actually redrawn (15 per full frame)
    int64_t text_us, wait_us, total_us; // summed over frames since last take
} render_stats_t;

void render_init(void);
void render_frame(const scene_t *s); // redraws only the rows that changed since the last frame
void render_invalidate(void);         // force the next frame to redraw everything
void render_set_rotation(int deg);    // 0 or 180
void render_take_stats(render_stats_t *out);
void render_picker(const picker_view_t *v); // full redraw; the next render_frame redraws everything
int render_task_width(const char *task);    // width of the task line in px
int render_pill_width(const char *name);    // width of the pill for this project name (cut to fit), px
// Debug: draw the scene (or, if v is given, the list) upright into out
// (360 x 360 RGB565, byte-swapped like the display's).
void render_capture(const scene_t *s, const picker_view_t *v, uint16_t *out);
