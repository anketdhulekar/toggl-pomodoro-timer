// Knob Timer: Toggl time tracking for the Waveshare ESP32-S3 Knob, in two modes.
//
// Tracking (the default) counts up; every running stretch is a Toggl entry.
// Pomodoro counts down from a length you set (1-60 min) and logs its stretches too.
//
//   Tap the screen   start / pause / resume (Pomodoro: dismiss "time's up")
//   Hold 0.7 s       reset (Tracking: to 00:00; Pomodoro: to the length you set)
//   Turn the knob    Tracking, running: clockwise adds a minute, anticlockwise takes one off
//                    Pomodoro: set the length while reset; whole minutes while paused
//   Tap the task     recent Toggl entries: pick one as the task ("Untitled" on top)
//   Tap the pill     Toggl projects ("No project" last)
//   Tap the mic      say the task (OpenAI speech-to-text)
//   Swipe up         switch Tracking / Pomodoro (not while running)
//   Swipe down       Tracking: check Toggl's running timer (manual sync)
//   Swipe left/right switch the color profile (Mono / Inverted), any time
// Toggl is only contacted on these actions; there is no background polling.
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "assets.h"
#include "board.h"
#include "cJSON.h"
#include "cloud.h"
#include "color.h"
#include "display.h"
#include "driver/i2c_master.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "haptics.h"
#include "input.h"
#include "mbedtls/base64.h"
#include "mic.h"
#include "net.h"
#include "render.h"
#include "settings.h"

static const char *TAG = "pomodoro";

#define UI_ROTATION 180 // degrees; the interface is shown upside down relative to the panel
#define MIN_US 60000000LL
#define MAX_MIN 60
#define HINT_US 1400000
#define MODE_HINT_US 1600000
#define SAVE_DELAY_US 2000000
#define PICKER_IDLE_US 10000000 // a list closes by itself after this long without input
#define MIC_HIT_R 34            // a tap this close to the mic presses it, px
#define MIC_DEAD_R 44           // below the status line, a tap this close (but not on it) only bumps
#define MIN_LISTEN_US 500000
#define MARQUEE_PAUSE_US 2000000 // a long task rests at its start this long before each pass
#define MARQUEE_PX_PER_S 30
// Tap targets (interface pixels, as in the preview): the task line and the project pill.
#define TASK_BOX_X0 40
#define TASK_BOX_X1 320
#define TASK_BOX_Y0 92
#define TASK_BOX_Y1 134
#define PILL_BOX_Y0 44
#define PILL_BOX_Y1 86
#define PILL_BOX_PAD 8
#define GRAY 0x8E8E94
#define ALARM_US 30000000 // time's up vibrates this long at full intensity, unless a touch or knob turn stops it

typedef enum { APP_TRACK, APP_POMO } app_t;
typedef enum { RUN_IDLE, RUN_RUNNING, RUN_PAUSED, RUN_DONE } run_t;

// Voice: tap the mic to listen; it stops after a pause in speech (or a tap)
// and the recording goes off for transcription.
typedef enum { VOICE_IDLE, VOICE_LISTENING, VOICE_WAITING } voice_t;

static struct {
    app_t app;
    int theme;
    // Tracking: the session total (earlier stretches) plus the running stretch.
    run_t t_state; // idle, running or paused
    int64_t t_session_us, t_start_us;
    // Pomodoro
    run_t p_state;
    int set_min;
    int64_t remaining_us;
    int64_t alarm_until; // time's up: vibrating until then (0 = quiet)
    int hint;
    int64_t hint_until;
    bool syncing;        // swipe-down check of Toggl's timer in flight
    int64_t dirty_since; // 0 = settings saved
    voice_t voice;
    int64_t listen_us;         // when listening started
    char task[SCENE_TASK_LEN]; // "" = untitled
    int64_t task_id;           // Toggl task the task name came from, 0 = none
    project_t project;         // id 0 = none; -1 = not chosen yet ("Work" once the projects load)
    bool telemetry;            // print TELEMETRY lines (debug; saved)
} S = {.set_min = 25, .hint = -1, .project = {.id = -1}};

// Toggl projects (a copy of the cloud task's list), recent entries, and the
// list screen that shows either of them.
typedef enum { PICK_PROJECTS, PICK_TASKS } pick_mode_t;
static struct {
    project_t list[MAX_PROJECTS];
    int count;
    uint32_t version;
    toggl_entry_t *entries; // PSRAM
    int nentries;
    entries_state_t entries_state;
    bool open;
    pick_mode_t mode;
    int idx;      // tasks: 0 = "Untitled", i = entries[i - 1]; projects: list[idx], count = "No project"
    float pos;    // scroll position, glides toward idx
    bool touched; // the knob moved since the list opened
    int64_t until;
} P;

static bool s_force_render; // redraw even if the scene looks unchanged (e.g. after a list)
static int s_status = -1;   // the status line last shown (debug)

static void show_hint(int id, int64_t now, int64_t dur)
{
    S.hint = id;
    S.hint_until = now + dur;
}

static void mark_dirty(int64_t now) { S.dirty_since = now; }

static void bump(int hint, int64_t now)
{
    haptics_play(HAP_BUMP);
    if (hint >= 0) show_hint(hint, now, HINT_US);
}

// Any touch or knob turn silences the time's-up vibration.
static void quiet(void)
{
    if (!S.alarm_until) return;
    S.alarm_until = 0;
    haptics_buzz(false);
    ESP_LOGI(TAG, "time's up: vibration stopped by a touch or the knob");
}

static int64_t proj_id(void) { return S.project.id > 0 ? S.project.id : 0; }
static run_t run_state(void) { return S.app == APP_TRACK ? S.t_state : S.p_state; }
static bool running(void) { return run_state() == RUN_RUNNING; }
static int64_t track_elapsed(int64_t now)
{
    return S.t_session_us + (S.t_state == RUN_RUNNING ? now - S.t_start_us : 0);
}

// Tracking counts up: MM:SS under an hour, HH:MM:SS from one hour on.
// Pomodoro counts down from at most 60 minutes and always shows MM:SS (60:00).
static void fmt_time(int64_t us, bool count_up, char out[9])
{
    int64_t s = count_up ? us / 1000000 : (us + 999999) / 1000000;
    if (s < 0) s = 0;
    if (s > 99 * 3600 + 3599) s = 99 * 3600 + 3599;
    unsigned h = (unsigned)(s / 3600) % 100, m = (unsigned)(s % 3600 / 60) % 60;
    const unsigned sec = (unsigned)(s % 60);
    if (!count_up) {
        h = 0;
        m = (unsigned)(s / 60) % 100;
    }
    char buf[16];
    if (h) snprintf(buf, sizeof buf, "%02u:%02u:%02u", h, m, sec);
    else snprintf(buf, sizeof buf, "%02u:%02u", m, sec);
    memcpy(out, buf, 9);
}

static void shown_time(int64_t now, char out[9])
{
    if (S.app == APP_TRACK) fmt_time(track_elapsed(now), true, out);
    else fmt_time(S.remaining_us, false, out);
}

// ---- Projects -------------------------------------------------------------------

// Select a project; with `push`, a running entry follows.
static void set_project(const project_t *p, bool push, int64_t now)
{
    if (p) S.project = *p;
    else memset(&S.project, 0, sizeof S.project);
    mark_dirty(now);
    if (push && running()) cloud_session_update(S.task, proj_id(), S.task_id);
    ESP_LOGI(TAG, "project: %s", S.project.id > 0 ? S.project.name : "none");
}

static const project_t *find_project(int64_t id)
{
    for (int i = 0; i < P.count; i++) {
        if (P.list[i].id == id) return &P.list[i];
    }
    return NULL;
}

// A hold also clears the task ("Untitled") and the project ("No project"), in both modes.
// Call it after the running entry is stopped: nothing goes to Toggl.
static bool task_project_cleared(void) { return !S.task[0] && !S.task_id && S.project.id == 0; }

static void clear_task_project(int64_t now)
{
    S.task[0] = '\0';
    S.task_id = 0;
    memset(&S.project, 0, sizeof S.project); // id 0: chosen "No project" (not -1, which becomes "Work")
    mark_dirty(now);
    ESP_LOGI(TAG, "hold: task and project cleared");
}

// Pick up a reloaded project list; keep the selection's name and color current.
static void sync_projects(int64_t now)
{
    const uint32_t v = cloud_projects_version();
    if (v == P.version) return;
    // An open projects list keeps its highlighted project (by id) when the list reloads.
    const bool listing = P.open && P.mode == PICK_PROJECTS;
    const int64_t highlighted = listing && P.idx < P.count ? P.list[P.idx].id : 0;
    P.count = cloud_projects(P.list, MAX_PROJECTS, &P.version);
    if (listing) {
        P.idx = P.count; // "No project"
        for (int i = 0; i < P.count && highlighted; i++) {
            if (P.list[i].id == highlighted) P.idx = i;
        }
        P.pos = (float)P.idx;
    }
    if (S.project.id < 0) { // never chosen: "Work" by default
        const project_t *work = NULL;
        for (int i = 0; i < P.count && !work; i++) {
            if (strcasecmp(P.list[i].name, "Work") == 0) work = &P.list[i];
        }
        if (work || P.count) set_project(work, true, now);
        return;
    }
    if (!S.project.id) return;
    // Keep the selection even if the list doesn't have it (the list only holds active
    // projects); just refresh its name and color when it's there.
    const project_t *p = find_project(S.project.id);
    if (p && (p->color != S.project.color || strcmp(p->name, S.project.name) != 0)) set_project(p, false, now);
}

// ---- Task -----------------------------------------------------------------------

// A task picked from the list. While tracking, a new task is a new entry (the
// counter keeps going); in a running Pomodoro the entry is renamed.
static void pick_task(const char *text, int64_t task_id, const project_t *proj, bool set_proj, int64_t now)
{
    if (strcmp(text, S.task) == 0 && task_id == S.task_id && (!set_proj || (proj ? proj->id : 0) == proj_id())) {
        ESP_LOGI(TAG, "task from the list: unchanged");
        return; // nothing to split or rename in Toggl
    }
    strlcpy(S.task, text, sizeof S.task);
    S.task_id = task_id;
    if (set_proj) {
        if (proj) S.project = *proj;
        else memset(&S.project, 0, sizeof S.project);
    }
    mark_dirty(now);
    if (S.app == APP_TRACK && S.t_state == RUN_RUNNING) {
        S.t_session_us += now - S.t_start_us;
        S.t_start_us = now;
        cloud_session_start(S.task, proj_id(), S.task_id, now); // (stops the running entry)
    } else if (running()) {
        cloud_session_update(S.task, proj_id(), S.task_id);
    }
    ESP_LOGI(TAG, "task from the list: \"%s\"", S.task[0] ? S.task : "(untitled)");
}

// ---- Lists ----------------------------------------------------------------------

static void list_show(pick_mode_t mode, int idx, int64_t now)
{
    P.mode = mode;
    P.idx = idx;
    P.pos = (float)idx;
    P.touched = false;
    P.open = true;
    P.until = now + PICKER_IDLE_US;
    haptics_play(HAP_TAP);
}

// Tap the pill: the projects, sorted by name, "No project" last.
static void projects_open(int64_t now)
{
    if (!cloud_toggl_configured()) {
        bump(STR_TOGGL_OFF, now);
        return;
    }
    cloud_refresh_projects();
    int idx = P.count; // "No project"
    for (int i = 0; i < P.count; i++) {
        if (P.list[i].id == S.project.id) idx = i;
    }
    list_show(PICK_PROJECTS, idx, now);
}

// Tap the task: recent entries, fetched fresh every time.
static void tasks_open(int64_t now)
{
    if (!cloud_toggl_configured()) {
        bump(STR_TOGGL_OFF, now);
        return;
    }
    P.entries_state = cloud_fetch_entries() ? ENTRIES_LOADING : ENTRIES_FAILED;
    P.nentries = 0;
    list_show(PICK_TASKS, 0, now);
}

static void entries_update(int64_t now)
{
    if (!P.open || P.mode != PICK_TASKS || P.entries_state != ENTRIES_LOADING) return;
    const entries_state_t st = cloud_entries(P.entries, MAX_ENTRIES, &P.nentries);
    if (st == ENTRIES_LOADING) return;
    P.entries_state = st;
    P.until = now + PICKER_IDLE_US;
    if (st == ENTRIES_FAILED) haptics_play(HAP_BUMP);
    if (st == ENTRIES_OK && P.nentries && !P.touched) P.idx = 1; // the newest entry; "Untitled" is one click up
}

static void picker_close(bool choose, int64_t now)
{
    P.open = false;
    s_force_render = true;
    haptics_play(HAP_TAP);
    if (!choose) return;
    if (P.mode == PICK_PROJECTS) {
        // With nothing loaded, or without a turn of the knob, a tap keeps the project: a
        // reflex tap mustn't clear it (the list opens on "No project" for an unlisted one).
        if (!P.count || !P.touched) return;
        const project_t *p = P.idx < P.count ? &P.list[P.idx] : NULL;
        if ((p ? p->id : 0) == proj_id() && S.project.id >= 0) return; // unchanged
        S.task_id = 0; // a Toggl task belongs to its project
        set_project(p, true, now);
    } else if (P.entries_state != ENTRIES_OK) {
        return; // still loading, or it failed: "Untitled" is only a placeholder
    } else if (P.idx == 0) {
        pick_task("", 0, NULL, false, now); // "Untitled"
    } else if (P.idx <= P.nentries) {
        const toggl_entry_t *e = &P.entries[P.idx - 1];
        const project_t *p = e->project.id ? find_project(e->project.id) : NULL;
        pick_task(e->label, e->task_id, p ? p : (e->project.id ? &e->project : NULL), true, now);
    }
}

// ---- Voice ----------------------------------------------------------------------

static bool mic_visible(void) { return cloud_voice_configured() && !P.open; }

static int dist2(int x, int y, int cx, int cy)
{
    const int dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy;
}

static void voice_fail(int hint, int64_t now)
{
    ESP_LOGW(TAG, "voice: %s", hint == STR_NOT_HEARD ? "didn't catch that" : hint == STR_NO_WIFI ? "no Wi-Fi"
                                                                                : "transcription failed");
    S.voice = VOICE_IDLE;
    bump(hint, now);
}

static void listen_start(int64_t now)
{
    if (!cloud_online()) {
        voice_fail(STR_NO_WIFI, now);
        return;
    }
    if (!mic_start()) {
        voice_fail(STR_STT_FAILED, now);
        return;
    }
    S.voice = VOICE_LISTENING;
    S.listen_us = now;
    haptics_play(HAP_TAP);
}

static void listen_stop(int64_t now)
{
    int16_t *pcm;
    const size_t n = mic_stop(&pcm);
    ESP_LOGI(TAG, "voice: listened %.1f s, sending %.1f s of audio", (now - S.listen_us) / 1e6, n / (float)MIC_RATE);
    if (now - S.listen_us < MIN_LISTEN_US) {
        voice_fail(STR_NOT_HEARD, now);
        return;
    }
    if (!cloud_transcribe(pcm, n)) {
        voice_fail(STR_STT_FAILED, now);
        return;
    }
    S.voice = VOICE_WAITING;
    haptics_play(HAP_CLICK);
}

static void voice_update(int64_t now)
{
    if (S.voice == VOICE_LISTENING) {
        if (mic_speech_done() || mic_full()) listen_stop(now);
        return;
    }
    if (S.voice != VOICE_WAITING) return;
    char text[SCENE_TASK_LEN];
    switch (cloud_take_transcript(text, sizeof text)) {
    case STT_PENDING: break;
    case STT_OK: {
        strlcpy(S.task, text, sizeof S.task);
        S.task_id = 0;
        mark_dirty(now);
        S.voice = VOICE_IDLE;
        haptics_play(HAP_TAP);
        // Naming a project selects it ("Acme design review" -> Acme).
        const project_t *p = find_project(cloud_match_project(S.task));
        if (p && p->id != S.project.id) set_project(p, false, now);
        if (running()) cloud_session_update(S.task, proj_id(), S.task_id); // renames the running entry
        break;
    }
    case STT_NO_SPEECH: voice_fail(STR_NOT_HEARD, now); break;
    case STT_NO_WIFI: voice_fail(STR_NO_WIFI, now); break;
    default: voice_fail(STR_STT_FAILED, now); break;
    }
}

// ---- Tracking -------------------------------------------------------------------

static void track_tap(int64_t now)
{
    if (S.t_state == RUN_RUNNING) { // pause: this stretch's entry stops
        S.t_session_us += now - S.t_start_us;
        S.t_state = RUN_PAUSED;
        cloud_session_stop(now);
    } else { // start, or resume as a new entry with the same task and project
        if (S.t_state == RUN_IDLE) S.t_session_us = 0;
        S.t_start_us = now;
        S.t_state = RUN_RUNNING;
        cloud_session_start(S.task, proj_id(), S.task_id, now);
    }
    haptics_play(HAP_TAP);
}

static void track_hold(int64_t now)
{
    if (S.t_state == RUN_IDLE && task_project_cleared()) {
        bump(-1, now);
        return;
    }
    if (S.t_state == RUN_RUNNING) cloud_session_stop(now);
    S.t_state = RUN_IDLE;
    S.t_session_us = 0;
    clear_task_project(now);
    show_hint(STR_RESET, now, HINT_US);
    haptics_play(HAP_DOUBLE);
}

// While running, the knob adjusts the time: clockwise adds a minute (the stretch started
// a minute earlier), anticlockwise takes one off (never past now). Toggl's entry follows.
static void track_turn(int dir, int64_t now)
{
    if (S.t_state != RUN_RUNNING) {
        bump(-1, now);
        return;
    }
    int64_t start = S.t_start_us - dir * MIN_US;
    if (start > now) start = now;
    if (start == S.t_start_us) {
        bump(-1, now);
        return;
    }
    S.t_start_us = start;
    cloud_session_shift(start);
    haptics_play(HAP_CLICK);
}

// Swipe down: one check of Toggl's running timer.
static void refresh(int64_t now)
{
    if (!cloud_toggl_configured()) {
        bump(STR_TOGGL_OFF, now);
        return;
    }
    if (S.syncing) return;
    if (!cloud_check_running(S.t_state == RUN_RUNNING)) {
        bump(STR_TOGGL_ERROR, now);
        return;
    }
    S.syncing = true;
    S.hint = -1;
}

static void sync_update(int64_t now)
{
    if (!S.syncing) return;
    toggl_running_t cur;
    const sync_result_t r = cloud_take_check(&cur);
    if (r == SYNC_PENDING || r == SYNC_IDLE) return;
    S.syncing = false;
    if (S.app != APP_TRACK) return; // (switched modes meanwhile)
    switch (r) {
    case SYNC_ADOPTED: { // a timer started elsewhere: show it and count on
        strlcpy(S.task, cur.desc, sizeof S.task);
        S.task_id = cur.task_id;
        const project_t *p = find_project(cur.project_id);
        if (p) S.project = *p;
        else if (cur.project_id) S.project = (project_t){.id = cur.project_id, .color = GRAY, .name = "Project"};
        else memset(&S.project, 0, sizeof S.project);
        S.t_session_us = 0;
        S.t_start_us = cur.start_us < now ? cur.start_us : now;
        S.t_state = RUN_RUNNING;
        mark_dirty(now);
        show_hint(STR_SYNCED, now, HINT_US);
        haptics_play(HAP_TAP);
        ESP_LOGI(TAG, "synced from Toggl: \"%s\", %lld s in", S.task, (now - S.t_start_us) / 1000000);
        break;
    }
    case SYNC_STOPPED: // the knob's entry was stopped elsewhere
        if (S.t_state == RUN_RUNNING) {
            S.t_session_us += now - S.t_start_us;
            S.t_state = RUN_PAUSED;
        }
        show_hint(STR_STOPPED_IN_TOGGL, now, HINT_US);
        haptics_play(HAP_TAP);
        ESP_LOGI(TAG, "stopped in Toggl");
        break;
    case SYNC_IN_SYNC:
        show_hint(STR_IN_SYNC, now, HINT_US);
        haptics_play(HAP_TAP);
        ESP_LOGI(TAG, "in sync with Toggl");
        break;
    default:
        bump(STR_TOGGL_ERROR, now);
        ESP_LOGW(TAG, "couldn't check Toggl's timer");
        break;
    }
}

// ---- Pomodoro -------------------------------------------------------------------

static void pomo_reset(void)
{
    S.p_state = RUN_IDLE;
    S.remaining_us = S.set_min * MIN_US;
}

static void pomo_finish(int64_t now)
{
    if (P.open) picker_close(false, now); // show "time's up"; the silencing tap mustn't pick a row
    cloud_session_stop(now);
    S.p_state = RUN_DONE;
    S.remaining_us = 0;
    haptics_buzz(true);
    S.alarm_until = now + ALARM_US;
    ESP_LOGI(TAG, "time's up: vibrating for 30 s");
}

static void pomo_tap(int64_t now)
{
    if (S.p_state == RUN_IDLE || S.p_state == RUN_PAUSED) {
        S.p_state = RUN_RUNNING;
        cloud_session_start(S.task, proj_id(), S.task_id, now);
    } else if (S.p_state == RUN_RUNNING) {
        S.p_state = RUN_PAUSED;
        cloud_session_stop(now);
    } else {
        pomo_reset();
    }
    haptics_play(HAP_TAP);
}

static void pomo_hold(int64_t now)
{
    if (S.p_state == RUN_IDLE && task_project_cleared()) {
        bump(-1, now);
        return;
    }
    if (S.p_state == RUN_RUNNING) cloud_session_stop(now);
    pomo_reset();
    clear_task_project(now);
    show_hint(STR_RESET, now, HINT_US);
    haptics_play(HAP_DOUBLE);
}

static void pomo_turn(int dir, int64_t now)
{
    if (S.p_state == RUN_DONE) {
        bump(-1, now);
        return;
    }
    if (S.p_state == RUN_RUNNING) { // the time is locked while counting down
        bump(STR_PAUSE_TO_ADJUST, now);
        return;
    }
    if (S.p_state == RUN_IDLE) {
        const int n = S.set_min + dir;
        if (n < 1 || n > MAX_MIN) {
            bump(n < 1 ? STR_MIN_1 : STR_MAX_60, now);
            return;
        }
        S.set_min = n;
        S.remaining_us = n * MIN_US;
        mark_dirty(now);
    } else { // paused: step to the next whole minute up or down (24:58 -> 25:00 or 24:00)
        const int64_t whole = S.remaining_us / MIN_US;
        const int64_t m = dir > 0 ? whole + 1 : S.remaining_us % MIN_US ? whole : whole - 1;
        if (m < 1 || m > MAX_MIN) {
            bump(m < 1 ? STR_MIN_1 : STR_MAX_60, now);
            return;
        }
        S.remaining_us = m * MIN_US;
    }
    haptics_play(HAP_CLICK);
}

// ---- Input ----------------------------------------------------------------------

static void turn(int dir, int64_t now)
{
    quiet();
    if (P.open) {
        P.until = now + PICKER_IDLE_US;
        P.touched = true;
        const int i = P.idx + dir;
        const int last = P.mode == PICK_PROJECTS ? P.count : P.nentries;
        if (i < 0 || i > last) {
            bump(-1, now);
            return;
        }
        P.idx = i;
        haptics_play(HAP_CLICK);
        return;
    }
    if (S.app == APP_TRACK) track_turn(dir, now);
    else pomo_turn(dir, now);
}

static void app_tap(int64_t now)
{
    if (S.app == APP_TRACK) track_tap(now);
    else pomo_tap(now);
}

static bool in_pill(int x, int y)
{
    const int half = render_pill_width(S.project.id > 0 ? S.project.name : "No project") / 2 + PILL_BOX_PAD;
    return x >= 180 - half && x <= 180 + half && y >= PILL_BOX_Y0 && y <= PILL_BOX_Y1;
}

static void tap(int64_t now)
{
    quiet();
    if (P.open) {
        picker_close(true, now);
        return;
    }
    if (S.voice == VOICE_LISTENING) {
        listen_stop(now);
        return;
    }
    int x, y;
    input_press_origin(&x, &y);
    const int d_mic = mic_visible() ? dist2(x, y, 180, RENDER_MIC_CY) : INT32_MAX;
    if (d_mic < MIC_HIT_R * MIC_HIT_R) {
        if (S.voice == VOICE_IDLE) listen_start(now);
        return;
    }
    if (x >= TASK_BOX_X0 && x <= TASK_BOX_X1 && y >= TASK_BOX_Y0 && y <= TASK_BOX_Y1) {
        tasks_open(now);
        return;
    }
    if (in_pill(x, y)) {
        projects_open(now);
        return;
    }
    if (d_mic < MIC_DEAD_R * MIC_DEAD_R && y > RENDER_STATUS_BOTTOM) {
        haptics_play(HAP_BUMP); // a near miss on the mic: don't start the timer
        return;
    }
    app_tap(now);
}

static void hold(int64_t now)
{
    quiet();
    if (P.open || S.voice == VOICE_LISTENING) return;
    if (S.app == APP_TRACK) track_hold(now);
    else pomo_hold(now);
}

// Left/right: the color profile (Mono <-> Inverted), any time.
static void swipe(int dir, int64_t now)
{
    (void)dir;
    quiet();
    if (P.open) return;
    S.theme = (S.theme + 1) % THEME_COUNT;
    show_hint(S.theme == THEME_MONO ? STR_MONO : STR_INVERTED, now, HINT_US);
    mark_dirty(now);
    haptics_play(HAP_TAP);
}

static void swipe_up(int64_t now) // switch modes
{
    quiet();
    if (S.voice == VOICE_LISTENING) return;
    if (P.open) {
        picker_close(false, now);
        return;
    }
    if (running()) {
        bump(STR_PAUSE_TO_SWITCH, now);
        return;
    }
    if (S.syncing) { // its answer belongs to Tracking
        bump(-1, now);
        return;
    }
    S.app = S.app == APP_TRACK ? APP_POMO : APP_TRACK;
    show_hint(S.app == APP_TRACK ? STR_TRACKING : STR_POMODORO, now, MODE_HINT_US);
    mark_dirty(now);
    haptics_play(HAP_TAP);
    ESP_LOGI(TAG, "mode: %s", S.app == APP_TRACK ? "tracking" : "pomodoro");
}

static void swipe_down(int64_t now) // refresh from Toggl (Tracking only)
{
    quiet();
    if (S.voice == VOICE_LISTENING) return;
    if (P.open) {
        picker_close(false, now);
        return;
    }
    if (S.app == APP_TRACK) refresh(now);
}

static void save_settings(void)
{
    settings_t st = {.app = S.app, .theme = S.theme, .set_min = S.set_min, .task_id = S.task_id,
                     .project = S.project, .telemetry = S.telemetry};
    strlcpy(st.task, S.task, sizeof st.task);
    settings_save(&st);
}

#ifdef CONFIG_KNOB_DEBUG_INTERFACE
#define DEBUG_INTERFACE 1
#else
#define DEBUG_INTERFACE 0
#endif

// Debug: print "<prefix><json>" as one write, so log lines from other tasks can't split it.
static void emit(const char *prefix, const char *json)
{
    const size_t n = strlen(prefix) + strlen(json) + 2;
    char *buf = malloc(n);
    if (!buf) return;
    snprintf(buf, n, "%s%s\n", prefix, json);
    fflush(stdout);
    write(fileno(stdout), buf, n - 1);
    free(buf);
}

static const char *const RUNS[] = {"idle", "running", "paused", "done"};

// Debug: one line with everything a test needs to know ("STATE {json}").
static void state_dump(void)
{
    static const char *const VOICES[] = {"idle", "listening", "waiting"};
    static const char *const ENTRIES[] = {"idle", "loading", "ok", "failed"};
    const int64_t now = esp_timer_get_time();
    char t[9];
    shown_time(now, t);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "app", S.app == APP_TRACK ? "track" : "pomo");
    cJSON_AddStringToObject(j, "mode", RUNS[run_state()]);
    cJSON_AddStringToObject(j, "time", t);
    cJSON_AddStringToObject(j, "track_state", RUNS[S.t_state]);
    cJSON_AddNumberToObject(j, "elapsed_s", (double)(track_elapsed(now) / 1000000));
    cJSON_AddStringToObject(j, "pomo_state", RUNS[S.p_state]);
    cJSON_AddNumberToObject(j, "remaining_s", (double)((S.remaining_us + 999999) / 1000000));
    cJSON_AddNumberToObject(j, "set_min", S.set_min);
    cJSON_AddStringToObject(j, "theme", S.theme == THEME_MONO ? "mono" : "inverted");
    cJSON_AddStringToObject(j, "voice", VOICES[S.voice]);
    cJSON_AddStringToObject(j, "task", S.task);
    cJSON_AddNumberToObject(j, "task_id", (double)S.task_id);
    cJSON_AddStringToObject(j, "project", S.project.id > 0 ? S.project.name : "");
    cJSON_AddNumberToObject(j, "project_id", (double)S.project.id);
    char hex[8];
    snprintf(hex, sizeof hex, "#%06lX", (unsigned long)(S.project.color & 0xFFFFFF));
    cJSON_AddStringToObject(j, "project_color", hex);
    cJSON_AddStringToObject(j, "list", !P.open ? "closed" : P.mode == PICK_PROJECTS ? "projects" : "tasks");
    cJSON_AddNumberToObject(j, "list_idx", P.idx);
    cJSON_AddNumberToObject(j, "projects", P.count);
    cJSON_AddNumberToObject(j, "entries", P.nentries);
    cJSON_AddStringToObject(j, "entries_state", ENTRIES[P.entries_state]);
    cJSON_AddBoolToObject(j, "syncing", S.syncing);
    cJSON_AddStringToObject(j, "status", s_status >= 0 && s_status < STR_COUNT ? STRING_TEXT[s_status] : "");
    cJSON_AddNumberToObject(j, "toggl_entry", (double)cloud_running_entry());
    cJSON_AddBoolToObject(j, "wifi", net_connected());
    cJSON_AddBoolToObject(j, "toggl", cloud_toggl_ready());
    cJSON_AddBoolToObject(j, "voice_configured", cloud_voice_configured());
    cJSON_AddBoolToObject(j, "mic_shown", mic_visible());
    cJSON_AddBoolToObject(j, "pulsing", S.app == APP_POMO && (S.p_state == RUN_DONE ||
                                                            (S.p_state == RUN_RUNNING && S.remaining_us <= MIN_US)));
    cJSON_AddBoolToObject(j, "alarm", S.alarm_until != 0);
    cJSON_AddBoolToObject(j, "telemetry", S.telemetry);
    cJSON_AddNumberToObject(j, "free_internal_kb", (double)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    char *txt = cJSON_PrintUnformatted(j);
    emit("STATE ", txt ? txt : "{}");
    cJSON_free(txt);
    cJSON_Delete(j);
}

// Debug: the screen as base64 RGB565 (byte-swapped), 768 bytes per line, between
// "CAPTURE 360 360" and "CAPTURE END". Written straight to the USB serial port (the
// console also mirrors to the slow hardware UART, which took ~30 s and tripped the
// watchdog), with logging paused so no log line lands inside the data.
static bool s_want_capture;
static void usb_puts(const char *s)
{
    // In pieces: the driver can't take a write bigger than its transmit buffer.
    for (size_t left = strlen(s); left > 0;) {
        const size_t n = left > 256 ? 256 : left;
        const int sent = usb_serial_jtag_write_bytes(s, n, pdMS_TO_TICKS(500));
        if (sent <= 0) return;
        s += sent;
        left -= sent;
    }
}

static void capture_send(const scene_t *sc, const picker_view_t *pv)
{
    static uint16_t *buf;
    const size_t total = LCD_H_RES * LCD_V_RES * sizeof(uint16_t);
    if (!buf) buf = heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
    if (!buf) return;
    render_capture(sc, pv, buf);
    esp_log_level_set("*", ESP_LOG_NONE);
    vTaskDelay(pdMS_TO_TICKS(20)); // let any log line in flight finish
    char head[40];
    snprintf(head, sizeof head, "\nCAPTURE %d %d\n", LCD_H_RES, LCD_V_RES);
    usb_puts(head);
    static char line[1100];
    int n_lines = 0;
    for (size_t off = 0; off < total; off += 768) {
        const size_t n = total - off < 768 ? total - off : 768;
        size_t olen = 0;
        mbedtls_base64_encode((unsigned char *)line, sizeof line - 2, &olen, (const uint8_t *)buf + off, n);
        line[olen] = '\n';
        line[olen + 1] = 0;
        usb_puts(line);
        if (++n_lines % 16 == 0) vTaskDelay(1); // let the idle task run (watchdog)
    }
    usb_puts("CAPTURE END\n");
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("i2c.master", ESP_LOG_NONE); // (as set at start-up)
}

// Debug: the project and entry lists as JSON ("PROJECTS [...]", "ENTRIES [...]").
static void list_dump(void)
{
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < P.count; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", (double)P.list[i].id);
        cJSON_AddStringToObject(o, "name", P.list[i].name);
        char hex[8];
        snprintf(hex, sizeof hex, "#%06lX", (unsigned long)P.list[i].color);
        cJSON_AddStringToObject(o, "color", hex);
        cJSON_AddItemToArray(a, o);
    }
    char *txt = cJSON_PrintUnformatted(a);
    emit("PROJECTS ", txt ? txt : "[]");
    cJSON_free(txt);
    cJSON_Delete(a);
    a = cJSON_CreateArray();
    for (int i = 0; i < P.nentries; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "label", P.entries[i].label);
        cJSON_AddNumberToObject(o, "task_id", (double)P.entries[i].task_id);
        cJSON_AddNumberToObject(o, "project_id", (double)P.entries[i].project.id);
        cJSON_AddItemToArray(a, o);
    }
    txt = cJSON_PrintUnformatted(a);
    emit("ENTRIES ", txt ? txt : "[]");
    cJSON_free(txt);
    cJSON_Delete(a);
}

// Telemetry: one JSON line every 5 s while enabled (#telemetry on). For debugging
// from the serial log; never includes keys or passwords.
static void telemetry(int64_t now)
{
    static int64_t last;
    if (!S.telemetry || now - last < 5000000) return;
    last = now;
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "uptime_s", (double)(now / 1000000));
    cJSON_AddStringToObject(j, "app", S.app == APP_TRACK ? "track" : "pomo");
    cJSON_AddStringToObject(j, "mode", RUNS[run_state()]);
    cJSON_AddNumberToObject(j, "elapsed_s", (double)(track_elapsed(now) / 1000000));
    cJSON_AddNumberToObject(j, "remaining_s", (double)((S.remaining_us + 999999) / 1000000));
    cJSON_AddNumberToObject(j, "voice", S.voice);
    cJSON_AddBoolToObject(j, "list_open", P.open);
    cJSON_AddNumberToObject(j, "free_internal_kb", (double)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    cJSON_AddNumberToObject(j, "min_free_internal_kb", (double)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024));
    cJSON_AddNumberToObject(j, "largest_internal_kb", (double)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
    cJSON_AddNumberToObject(j, "free_psram_kb", (double)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    cJSON_AddBoolToObject(j, "wifi", net_connected());
    cJSON_AddNumberToObject(j, "rssi", net_rssi());
    cJSON_AddBoolToObject(j, "clock_synced", net_time_ok());
    cloud_debug_fields(j);
    char *txt = cJSON_PrintUnformatted(j);
    emit("TELEMETRY ", txt ? txt : "{}");
    cJSON_free(txt);
    cJSON_Delete(j);
}

// Debug: "#name args" line commands, for tests that need exact inputs or state.
//   #tap X Y          tap at interface pixel (X, Y) (hit areas, dead zones)
//   #task TEXT        set the task name ("#task" alone clears it)
//   #project ID [#RRGGBB NAME]  select a project by Toggl id (0 = none); color and name for one not in the list
//   #set N            Pomodoro length in minutes (while reset)
//   #app track|pomo   switch modes (not while running)
//   #theme mono|inverted
//   #session N        Tracking total in seconds, paused (not while running; Toggl isn't told)
//   #remaining N      Pomodoro time left in seconds, paused (not while running)
//   #toggl start TEXT / #toggl stop   start or stop a timer in Toggl directly, as another device would
//   #wifi on|off      drop / restore Wi-Fi (offline behavior)
//   #telemetry on|off TELEMETRY lines every 5 s (saved)
//   #list             print the project and entry lists
//   #reboot           save the settings and restart
// Each answers "CMD ok ..." or "CMD error ...".
static void line_command(char *cmd, int64_t now)
{
    char *arg = strchr(cmd, ' ');
    if (arg) *arg++ = 0;
    else arg = cmd + strlen(cmd);
    bool ok = true;
    if (!strcmp(cmd, "tap")) {
        int x, y;
        ok = sscanf(arg, "%d %d", &x, &y) == 2;
        if (ok) {
            input_debug_origin(x, y);
            tap(now);
        }
    } else if (!strcmp(cmd, "task")) {
        strlcpy(S.task, arg, sizeof S.task);
        S.task_id = 0;
        mark_dirty(now);
    } else if (!strcmp(cmd, "task_id")) { // the Toggl task the task name belongs to (restoring settings)
        S.task_id = strtoll(arg, NULL, 10);
        mark_dirty(now);
    } else if (!strcmp(cmd, "project")) {
        // "#project ID", or "#project ID #RRGGBB NAME" for a project that isn't in the list (inactive)
        char *rest;
        const int64_t id = strtoll(arg, &rest, 10);
        const project_t *p = find_project(id);
        project_t given = {.id = id};
        if (!p && id > 0 && sscanf(rest, " #%6" SCNx32, &given.color) == 1 && strlen(rest) > 9) {
            strlcpy(given.name, rest + 9, sizeof given.name);
            p = &given;
        }
        ok = id == 0 || p;
        if (ok) set_project(id ? p : NULL, true, now);
    } else if (!strcmp(cmd, "set")) {
        const int n = atoi(arg);
        ok = S.p_state == RUN_IDLE && n >= 1 && n <= MAX_MIN;
        if (ok) {
            S.set_min = n;
            pomo_reset();
            mark_dirty(now);
        }
    } else if (!strcmp(cmd, "app")) {
        ok = !running() && (!strcmp(arg, "track") || !strcmp(arg, "pomo"));
        if (ok) {
            S.app = !strcmp(arg, "track") ? APP_TRACK : APP_POMO;
            mark_dirty(now);
        }
    } else if (!strcmp(cmd, "theme")) {
        ok = !strcmp(arg, "mono") || !strcmp(arg, "inverted");
        if (ok) {
            S.theme = !strcmp(arg, "mono") ? THEME_MONO : THEME_INVERTED;
            mark_dirty(now);
        }
    } else if (!strcmp(cmd, "remaining")) {
        const long long secs = atoll(arg);
        ok = S.p_state != RUN_RUNNING && secs >= 1 && secs <= MAX_MIN * 60;
        if (ok) {
            S.remaining_us = secs * 1000000LL;
            S.p_state = RUN_PAUSED;
        }
    } else if (!strcmp(cmd, "session")) {
        const long long secs = atoll(arg);
        ok = S.t_state != RUN_RUNNING && secs >= 0;
        if (ok) {
            S.t_session_us = secs * 1000000LL;
            S.t_state = secs ? RUN_PAUSED : RUN_IDLE;
        }
    } else if (!strcmp(cmd, "toggl")) {
        if (!strncmp(arg, "start", 5)) cloud_debug_start_elsewhere(arg[5] == ' ' ? arg + 6 : "knob-test elsewhere");
        else if (!strcmp(arg, "stop")) cloud_debug_stop_elsewhere();
        else ok = false;
    } else if (!strcmp(cmd, "wifi")) {
        net_debug_enable(strcmp(arg, "off") != 0);
    } else if (!strcmp(cmd, "telemetry")) {
        S.telemetry = strcmp(arg, "off") != 0;
        mark_dirty(now);
    } else if (!strcmp(cmd, "list")) {
        list_dump();
    } else if (!strcmp(cmd, "reboot")) {
        printf("CMD ok reboot\n");
        save_settings();
        esp_restart();
    } else {
        ok = false;
    }
    char reply[160];
    snprintf(reply, sizeof reply, "%s %s %s", ok ? "ok" : "error", cmd, arg);
    emit("CMD ", reply);
}

// Debug: single-letter commands over the USB serial port act like the controls,
// so the flows can be tested from a computer.
//   s = tap the screen (start / pause), r = hold (reset), + / - = turn the knob,
//   e = tap the task (past tasks), o = tap the pill (projects), m = tap the mic,
//   t = tap inside a list (choose) or stop listening, u / p = swipe up (mode) / down (refresh),
//   < / > = swipe left / right (profile), d = print the state, c = capture the screen
static void serial_commands(int64_t now)
{
    static char line[128];
    static int len = -1; // -1: not inside a "#..." line command
    uint8_t c;
    while (usb_serial_jtag_read_bytes(&c, 1, 0) == 1) {
        if (len >= 0) {
            if (c == '\n' || c == '\r') {
                line[len] = 0;
                len = -1;
                line_command(line, now);
            } else if (len < (int)sizeof line - 1) {
                line[len++] = (char)c;
            }
            continue;
        }
        if (c == '#') {
            len = 0;
            continue;
        }
        if (c == '\n' || c == '\r') continue;
        ESP_LOGI(TAG, "serial command '%c'", c);
        switch (c) {
        case 'm':
            if (mic_visible() && S.voice == VOICE_IDLE) listen_start(now);
            break;
        case 'e':
            if (!P.open && S.voice != VOICE_LISTENING) tasks_open(now);
            break;
        case 'o':
            if (!P.open && S.voice != VOICE_LISTENING) projects_open(now);
            break;
        case 's':
            quiet();
            if (!P.open && S.voice != VOICE_LISTENING) app_tap(now);
            break;
        case 'r': hold(now); break;
        case '+': turn(+1, now); break;
        case '-': turn(-1, now); break;
        case '<': swipe(+1, now); break;
        case '>': swipe(-1, now); break;
        case 'u': swipe_up(now); break;
        case 'p': swipe_down(now); break;
        case 'd': state_dump(); break;
        case 'c': s_want_capture = true; break;
        case 't':
            if (P.open) picker_close(true, now);
            else if (S.voice == VOICE_LISTENING) listen_stop(now);
            break;
        default: break;
        }
    }
}

static bool fdiff(float a, float b, float tol) { return fabsf(a - b) > tol; }

static bool scene_differs(const scene_t *a, const scene_t *b)
{
    return a->theme != b->theme || a->pomo != b->pomo || fdiff(a->rim_frac, b->rim_frac, 0.0002f) ||
           a->rim_rgb != b->rim_rgb || a->has_project != b->has_project || a->project_rgb != b->project_rgb ||
           strcmp(a->project, b->project) != 0 || rgb_to_565(a->live_rgb) != rgb_to_565(b->live_rgb) ||
           strcmp(a->time, b->time) != 0 || fdiff(a->time_alpha, b->time_alpha, 0.01f) ||
           fdiff(a->time_scale, b->time_scale, 0.002f) || fdiff(a->glow, b->glow, 0.01f) ||
           strcmp(a->task, b->task) != 0 || a->task_untitled != b->task_untitled ||
           fdiff(a->task_alpha, b->task_alpha, 0.01f) || a->task_scroll != b->task_scroll || a->status != b->status ||
           fdiff(a->status_alpha, b->status_alpha, 0.01f) || a->wave_on != b->wave_on ||
           memcmp(a->wave, b->wave, sizeof a->wave) != 0 || fdiff(a->mic_alpha, b->mic_alpha, 0.01f) ||
           fdiff(a->mic_ring_alpha, b->mic_ring_alpha, 0.02f) || a->wifi_rim != b->wifi_rim ||
           fdiff(a->wifi_alpha, b->wifi_alpha, 0.02f) || fdiff(a->hold_progress, b->hold_progress, 0.01f);
}

#define MIC_SELFTEST 0 // set to 1 to record 2 s at boot and log the microphone levels

static i2c_master_bus_handle_t i2c_init(void)
{
    const i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &bus));
    // The touch chip sleeps between touches and NACKs polls; that is expected.
    esp_log_level_set("i2c.master", ESP_LOG_NONE);
    return bus;
}

void app_main(void)
{
    if (DEBUG_INTERFACE) { // USB input for test commands (see serial_commands)
        usb_serial_jtag_driver_config_t usj = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        usj.tx_buffer_size = 4096; // room for screen captures
        if (usb_serial_jtag_driver_install(&usj) == ESP_OK) usb_serial_jtag_vfs_use_driver();
    }
    settings_init();
    {
        settings_t st = {.app = S.app, .theme = S.theme, .set_min = S.set_min, .project = S.project};
        settings_load(&st);
        S.app = st.app == APP_POMO ? APP_POMO : APP_TRACK;
        S.theme = st.theme >= 0 && st.theme < THEME_COUNT ? st.theme : THEME_MONO;
        S.set_min = st.set_min;
        strlcpy(S.task, st.task, sizeof S.task);
        S.task_id = st.task_id;
        S.project = st.project;
        S.telemetry = st.telemetry;
    }
    P.entries = heap_caps_calloc(MAX_ENTRIES, sizeof(toggl_entry_t), MALLOC_CAP_SPIRAM);
    if (S.set_min < 1 || S.set_min > MAX_MIN) S.set_min = 25;
    pomo_reset();
    ESP_LOGI(TAG, "starting: %s, %s, %d min, task \"%s\", project %s", S.app == APP_TRACK ? "tracking" : "pomodoro",
             S.theme == THEME_MONO ? "mono" : "inverted", S.set_min, S.task,
             S.project.id > 0 ? S.project.name : S.project.id < 0 ? "default" : "none");

    i2c_master_bus_handle_t bus = i2c_init();
    display_init();
    render_init();
    render_set_rotation(UI_ROTATION);
    input_init(bus);
    input_set_rotation(UI_ROTATION);
    haptics_init(bus);
    mic_init();
#if MIC_SELFTEST
    { // record 2 s at boot and log the levels
        int16_t *pcm;
        mic_start();
        vTaskDelay(pdMS_TO_TICKS(2000));
        size_t n = mic_stop(&pcm);
        int lo = 0, hi = 0;
        for (size_t i = 0; i < n; i++) {
            if (pcm[i] < lo) lo = pcm[i];
            if (pcm[i] > hi) hi = pcm[i];
        }
        ESP_LOGI(TAG, "mic self-test: %u samples, raw range %d..%d", (unsigned)n, lo, hi);
        ESP_LOGI(TAG, "mic self-test: speech %s", mic_prepare(&pcm, &n) ? "detected" : "not detected");
    }
#endif
    net_init();
    cloud_init();
    ESP_LOGI(TAG, "free RAM: %u KB internal, %u KB PSRAM", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    vTaskDelay(pdMS_TO_TICKS(500));
    input_take_knob_steps(); // drop any power-up glitch on the knob lines

    lab_t base = rgb_to_oklab(THEMES[S.theme].ink);
    float shown_rim = S.app == APP_TRACK ? 1.0f : fminf(1, S.remaining_us / (60.0f * MIN_US));
    int64_t last = esp_timer_get_time(), pulse_start = -1, stats_since = last;
    scene_t prev = {0};
    bool first = true;
    float mic_lvl = 0;
    float wave_lv[21] = {0};
    int64_t wave_t = 0;
    char marquee_task[SCENE_TASK_LEN] = "";
    int64_t marquee_since = 0;
    int marquee_w = 0;
    picker_view_t pv;
    float pk_drawn_pos = -1;
    int pk_drawn_idx = -1, pk_drawn_count = -1, pk_drawn_theme = -1;

    while (true) {
        const int64_t now = esp_timer_get_time();
        const int64_t dt = now - last;
        const float dt_ms = dt / 1000.0f;
        last = now;

        // Input
        int steps = input_take_knob_steps();
        for (; steps > 0; steps--) turn(+1, now);
        for (; steps < 0; steps++) turn(-1, now);
        const touch_event_t ev = input_poll_touch(now);
        const press_state_t *press = input_press();
        switch (ev) {
        case TOUCH_TAP: tap(now); break;
        case TOUCH_HOLD: hold(now); break;
        case TOUCH_SWIPE_LEFT: swipe(+1, now); break;
        case TOUCH_SWIPE_RIGHT: swipe(-1, now); break;
        case TOUCH_SWIPE_DOWN: swipe_down(now); break;
        case TOUCH_SWIPE_UP: swipe_up(now); break;
        default: break;
        }
        if (DEBUG_INTERFACE) {
            serial_commands(now);
            telemetry(now);
        }
        cloud_tick();
        if (cloud_take_dropped()) bump(STR_NOT_SAVED, now);
        voice_update(now);
        sync_projects(now);
        entries_update(now);
        sync_update(now);
        if (P.open && now > P.until) picker_close(false, now);

        // Clock
        if (S.app == APP_POMO && S.p_state == RUN_RUNNING) {
            S.remaining_us -= dt;
            if (S.remaining_us <= 0) pomo_finish(now);
        }
        if (S.alarm_until && (S.p_state != RUN_DONE || now >= S.alarm_until)) {
            S.alarm_until = 0;
            haptics_buzz(false);
            ESP_LOGI(TAG, "time's up: vibration done");
        }
        if (S.dirty_since && now - S.dirty_since > SAVE_DELAY_US) {
            save_settings();
            S.dirty_since = 0;
        }

        // A list is drawn instead of the face while open.
        if (P.open) {
            P.pos += (P.idx - P.pos) * (1 - expf(-dt_ms / 70));
            if (fabsf(P.idx - P.pos) < 0.002f) P.pos = (float)P.idx;
            const int content = P.mode == PICK_PROJECTS ? P.count : P.nentries * 4 + P.entries_state;
            if (P.pos != pk_drawn_pos || P.idx != pk_drawn_idx || content != pk_drawn_count || S.theme != pk_drawn_theme) {
                pv.theme = S.theme;
                if (P.mode == PICK_PROJECTS) {
                    pv.count = P.count + 1;
                    for (int i = 0; i < P.count; i++) {
                        pv.names[i] = P.list[i].name;
                        pv.colors[i] = P.list[i].color;
                        pv.hollow[i] = false;
                    }
                    pv.names[P.count] = "No project";
                    pv.hollow[P.count] = true;
                    pv.hint = P.count ? STR_TAP_TO_CHOOSE : STR_NO_PROJECTS;
                } else {
                    pv.count = P.nentries + 1;
                    pv.names[0] = "Untitled";
                    pv.hollow[0] = true;
                    for (int i = 0; i < P.nentries; i++) {
                        pv.names[i + 1] = P.entries[i].label;
                        pv.colors[i + 1] = P.entries[i].project.color;
                        pv.hollow[i + 1] = !P.entries[i].project.id;
                    }
                    pv.hint = P.entries_state == ENTRIES_LOADING ? STR_LOADING
                              : P.entries_state == ENTRIES_FAILED ? STR_TOGGL_ERROR
                              : P.nentries ? STR_TAP_TO_CHOOSE
                                           : STR_NO_ENTRIES;
                }
                pv.selected = P.idx;
                pv.pos = P.pos;
                render_picker(&pv);
                pk_drawn_pos = P.pos;
                pk_drawn_idx = P.idx;
                pk_drawn_count = content;
                pk_drawn_theme = S.theme;
            }
            if (s_want_capture) {
                s_want_capture = false;
                capture_send(NULL, &pv);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        pk_drawn_pos = -1;

        const bool track = S.app == APP_TRACK;
        const theme_t *th = &THEMES[S.theme];
        scene_t sc = {.theme = S.theme, .pomo = !track, .time_scale = 1, .status_alpha = 1};

        // Rim: a full ring in Tracking; in Pomodoro an arc of the time left out of 60 min, gliding to knob changes.
        const float target = track ? 1.0f : fmaxf(0, fminf(1, S.remaining_us / (60.0f * MIN_US)));
        shown_rim += (target - shown_rim) * (1 - expf(-dt_ms / 70));
        if (fabsf(target - shown_rim) < 0.0005f) shown_rim = target;
        sc.rim_frac = shown_rim;
        sc.has_project = S.project.id > 0;
        sc.rim_rgb = sc.has_project ? S.project.color : !track ? 0xFFFFFF : th->ink;
        sc.project_rgb = S.project.color;
        if (sc.has_project) strlcpy(sc.project, S.project.name, sizeof sc.project);

        // Live color: the profile's ink (gliding on a profile swap); in Pomodoro's last ten
        // minutes it warms to orange, then red.
        base = lab_mix(base, rgb_to_oklab(th->ink), 1 - expf(-dt_ms / 120));
        sc.live_rgb = oklab_to_rgb(track ? base : warm_color(base, S.remaining_us / (float)MIN_US));

        shown_time(now, sc.time);
        const bool paused = run_state() == RUN_PAUSED;
        const bool pulsing = !track && (S.p_state == RUN_DONE || (S.p_state == RUN_RUNNING && S.remaining_us <= MIN_US));
        sc.time_alpha = paused ? 0.55f : 1;
        if (pulsing) {
            // Heartbeat: quick swell with a glow, then settle, once a second.
            if (pulse_start < 0) pulse_start = now;
            const float p = fmodf((now - pulse_start) / 1e6f, 1.0f);
            const float f = p < 0.18f ? sinf(p / 0.18f * (float)M_PI / 2)
                                      : 0.5f + 0.5f * cosf((p - 0.18f) / 0.82f * (float)M_PI);
            sc.time_scale = 1 + 0.07f * f;
            sc.glow = 0.85f * f;
        } else {
            pulse_start = -1;
        }

        const bool hinting = S.hint >= 0 && now < S.hint_until;
        const bool voice_busy = S.voice == VOICE_LISTENING || S.voice == VOICE_WAITING;
        const run_t rs = run_state();
        sc.status = hinting                       ? S.hint
                    : S.voice == VOICE_LISTENING ? -1 // the waveform shows instead
                    : S.voice == VOICE_WAITING   ? STR_TRANSCRIBING
                    : S.syncing                  ? STR_SYNCING
                    : rs == RUN_IDLE             ? (track ? STR_TAP_TO_TRACK : STR_TAP_TO_START)
                    : rs == RUN_PAUSED           ? STR_PAUSED
                    : rs == RUN_DONE             ? STR_TIMES_UP
                                                 : -1;
        s_status = sc.status;
        if ((paused || voice_busy || S.syncing) && !hinting) {
            const float b = sinf((float)M_PI * fmodf(now / 1e6f, 2.0f) / 2.0f);
            sc.status_alpha = 1 - 0.65f * b * b;
        }

        // Task (under the pill): "Untitled" in the secondary color when there's none. A task wider
        // than the window scrolls: it rests at the start, then glides through in a loop.
        sc.task_untitled = !S.task[0];
        strlcpy(sc.task, S.task[0] ? S.task : "Untitled", sizeof sc.task);
        sc.task_alpha = paused ? 0.55f : 1;
        if (strcmp(marquee_task, sc.task) != 0) {
            strlcpy(marquee_task, sc.task, sizeof marquee_task);
            marquee_since = now;
            marquee_w = render_task_width(sc.task);
        }
        sc.task_scroll = -1;
        if (marquee_w > TASK_MAX_W) {
            const int64_t pass_us = (int64_t)(marquee_w + TASK_SCROLL_GAP) * 1000000 / MARQUEE_PX_PER_S;
            const int64_t t = (now - marquee_since) % (MARQUEE_PAUSE_US + pass_us);
            sc.task_scroll = t < MARQUEE_PAUSE_US ? 0 : (int)((t - MARQUEE_PAUSE_US) * MARQUEE_PX_PER_S / 1000000);
        }

        // Mic: dim at rest, solid while listening, its ring following the voice level.
        mic_lvl += (mic_level() - mic_lvl) * (1 - expf(-dt_ms / 80));
        const bool mic_on = mic_visible();
        sc.mic_alpha = !mic_on ? 0 : S.voice == VOICE_LISTENING ? 1 : 0.8f;
        sc.mic_ring_alpha = !mic_on ? 0 : S.voice == VOICE_LISTENING ? 0.45f + 0.55f * mic_lvl : 0.45f;
        // Waveform while listening: the last ~1.3 s of mic level, one bar per 60 ms.
        if (S.voice == VOICE_LISTENING) {
            if (now - wave_t >= 60000) {
                wave_t = now;
                memmove(wave_lv, wave_lv + 1, sizeof wave_lv - sizeof wave_lv[0]);
                wave_lv[20] = sqrtf(mic_level());
            }
            sc.wave_on = true;
            for (int i = 0; i < 21; i++) sc.wave[i] = (uint8_t)(3 + 14 * wave_lv[i]);
        } else {
            memset(wave_lv, 0, sizeof wave_lv);
        }
        // Wi-Fi: a glyph in the rim at 6 o'clock, steady when connected, pulsing while (re)connecting.
        sc.wifi_rim = net_configured();
        if (sc.wifi_rim) {
            const float b = sinf((float)M_PI * fmodf(now / 1e6f, 2.0f) / 2.0f);
            sc.wifi_alpha = net_connected() ? 1 : 0.25f + 0.75f * b * b;
        }
        if (press->active && !press->moved && !press->fired) {
            const float t = (now - press->start_us - 120000) / (float)(INPUT_HOLD_US - 120000);
            sc.hold_progress = t < 0 ? 0 : (t > 1 ? 1 : t);
        }

        if (s_want_capture) {
            s_want_capture = false;
            capture_send(&sc, NULL);
            s_force_render = true;
        }
        if (first || s_force_render || scene_differs(&sc, &prev)) {
            s_force_render = false;
            render_frame(&sc);
            prev = sc;
            if (first) {
                display_wait_idle();
                display_set_backlight(255);
                first = false;
            }
        }
        if (now - stats_since > 10000000) {
            render_stats_t st;
            render_take_stats(&st);
            if (st.frames) {
                ESP_LOGI(TAG, "%d frames in 10 s: %.1f ms, %.1f bands each (text %.1f, DMA wait %.1f, pixels %.1f)",
                         st.frames, st.total_us / 1000.0 / st.frames, (double)st.bands / st.frames,
                         st.text_us / 1000.0 / st.frames, st.wait_us / 1000.0 / st.frames,
                         (st.total_us - st.text_us - st.wait_us) / 1000.0 / st.frames);
            }
            stats_since = now;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
