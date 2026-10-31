#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "project.h"

#define SCENE_TASK_LEN 96 // bytes of UTF-8, including the terminator
#define TASK_MAX_W 230    // px; a longer task scrolls (marquee)
#define TASK_SCROLL_GAP 48 // px between the end of a scrolling task and its next pass

// Everything the screen shows, in display pixels (360x360, center 180,180).
typedef struct {
    int palette;
    uint32_t live_rgb;   // slice, hands and off-slice text (0xRRGGBB)
    float slice_deg;     // time left as degrees clockwise from 12 (60 min = 360)
    char digits[4];      // "MMSS"
    float digit_alpha;   // 1, or 0.55 while paused
    float colon_alpha;   // ticks at 1 Hz while running
    float digit_scale;   // last-minute pulse swell (1 = rest)
    float glow;          // last-minute pulse glow (0..1)
    int status;          // str_id_t, or -1 for none
    float status_alpha;
    float dots_alpha;    // profile dots after a swipe
    float hold_progress; // long-press ring, 0 = hidden
    char task[SCENE_TASK_LEN]; // spoken task name above the digits (UTF-8), "" = none
    float task_alpha;
    int task_scroll;           // -1 = fits (centered); otherwise the marquee offset in px
    char project[PROJECT_NAME_LEN]; // project name in small type under the task, "" = none
    float project_alpha;
    bool rim_on;                // project selected: the band behind the ticks takes its color
    uint32_t rim_rgb;
    float mic_alpha;            // mic button under the time, 0 = hidden
    float mic_ring_alpha;
    float top_alpha;            // recent-entries badge at 12 o'clock (covers the hand), 0 = hidden
    bool wifi_rim;              // the Wi-Fi glyph replaces the 6 o'clock tick
    float wifi_alpha;           // its strength (pulses while reconnecting)
    bool wave_on;               // waveform on the status line while listening
    uint8_t wave[21];           // bar heights in px
} scene_t;

#define RENDER_TOP_CY 60 // recent-entries badge center (x = 180)
#define RENDER_MIC_CY 298

// Full-screen project list, scrolled with the knob.
#define PICKER_MAX 51 // rows: projects (+ "No project") or recent Toggl entries (+ "No task")
typedef struct {
    int count;
    const char *names[PICKER_MAX];
    uint32_t colors[PICKER_MAX]; // dot colors
    bool hollow[PICKER_MAX];     // drawn as an empty gray ring (no project)
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
void render_picker(const picker_view_t *v); // full redraw; the next render_frame redraws the dial in full
int render_task_width(const char *task);    // width of the task line in px, uncut
// Debug: draw the scene (or, if v is given, the list) upright into out
// (360 x 360 RGB565, byte-swapped like the display's).
void render_capture(const scene_t *s, const picker_view_t *v, uint16_t *out);
