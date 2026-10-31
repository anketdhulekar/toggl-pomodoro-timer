// Knob Pomodoro: a single adjustable countdown for the Waveshare ESP32-S3 Knob.
//
//   Turn the knob   add or remove a minute (1-60) while paused or reset; locked while running
//   Tap the screen  start / pause / resume; dismiss "time's up"
//   Hold 0.7 s      reset to the length you set
//   Tap the mic     (while stopped) say the task; OpenAI speech-to-text puts it above the time
//   Tap the clock   (while stopped, right of the mic) pick a recent Toggl entry as the task
//   Swipe down      (while stopped) pick a Toggl project; the rim takes its color
//   Swipe left/right (while stopped)  change the slice's color profile
// Running time is logged to Toggl: one entry per stretch between start and pause.
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
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
#define HINT_US 1200000
#define DOTS_US 1500000
#define FADE_US 400000.0f
#define SAVE_DELAY_US 2000000
#define PICKER_IDLE_US 10000000 // the project list closes by itself after this long without input
// Buttons: the mic under the time, the recent-entries badge at 12 o'clock.
#define BTN_HIT_R 38            // a tap this close to a button presses it, px
#define BTN_DEAD_R 46           // a tap this close (but not on it) does nothing: near misses don't start the timer
#define MIN_LISTEN_US 500000
#define MARQUEE_PAUSE_US 2000000 // a long task rests at its start this long before each pass
#define MARQUEE_PX_PER_S 30

typedef enum { RUN_IDLE, RUN_RUNNING, RUN_PAUSED, RUN_DONE } run_mode_t;

// Voice: tap the mic to listen; it stops after a pause in speech (or a second
// tap) and the recording goes off for transcription.
typedef enum { VOICE_IDLE, VOICE_LISTENING, VOICE_WAITING } voice_t;

static struct {
    int set_min;
    int palette;
    int64_t remaining_us;
    run_mode_t mode;
    int hint;
    int64_t hint_until;
    int64_t dots_start, dots_until;
    int64_t dirty_since; // 0 = settings saved
    voice_t voice;
    int64_t listen_us;         // when listening started
    char task[SCENE_TASK_LEN]; // task name (spoken, or from a Toggl entry), "" = none
    int64_t task_id;           // Toggl task the task name came from, 0 = none
    project_t project;         // selected Toggl project, id 0 = none
    bool telemetry;            // print TELEMETRY lines (debug; saved)
} S = {.set_min = 25, .hint = -1};

// Toggl projects (a copy of the cloud task's list), recent entries, and the
// list screen that shows either of them.
typedef enum { PICK_PROJECTS, PICK_ENTRIES } pick_mode_t;
static struct {
    project_t list[MAX_PROJECTS];
    int count;
    uint32_t version;
    toggl_entry_t *entries; // PSRAM
    int nentries;
    entries_state_t entries_state;
    bool open;
    pick_mode_t mode;
    int idx;      // 0 = "No project" / "No task", i = list[i - 1] / entries[i - 1]
    float pos;    // scroll position, glides toward idx
    bool touched; // the knob moved since the list opened
    int64_t until;
} P;

static bool s_force_render; // redraw the dial even if the scene looks unchanged (e.g. after the picker)

static void show_hint(int id, int64_t now)
{
    S.hint = id;
    S.hint_until = now + HINT_US;
}

static void mark_dirty(int64_t now) { S.dirty_since = now; }

static void bump(int hint, int64_t now)
{
    haptics_play(HAP_BUMP);
    if (hint >= 0) show_hint(hint, now);
}

static void reset_to_set(void)
{
    S.mode = RUN_IDLE;
    S.remaining_us = S.set_min * MIN_US;
}

static void finish(int64_t now)
{
    cloud_session_stop(now);
    S.mode = RUN_DONE;
    S.remaining_us = 0;
    haptics_play(HAP_ALARM);
    ESP_LOGI(TAG, "time's up");
}

// ---- Projects -------------------------------------------------------------------

static void select_project(const project_t *p, int64_t now)
{
    if (p) S.project = *p;
    else memset(&S.project, 0, sizeof S.project);
    mark_dirty(now);
    ESP_LOGI(TAG, "project: %s", S.project.id ? S.project.name : "none");
}

static const project_t *find_project(int64_t id)
{
    for (int i = 0; i < P.count; i++) {
        if (P.list[i].id == id) return &P.list[i];
    }
    return NULL;
}

// Pick up a reloaded project list; keep the selection's name and color current.
static void sync_projects(int64_t now)
{
    const uint32_t v = cloud_projects_version();
    if (v == P.version) return;
    P.count = cloud_projects(P.list, MAX_PROJECTS, &P.version);
    if (P.open && P.idx > P.count) P.idx = P.count;
    if (!S.project.id) return;
    // Keep the selection even if the list doesn't have it (the list only holds active
    // projects); just refresh its name and color when it's there.
    const project_t *p = find_project(S.project.id);
    if (p && (p->color != S.project.color || strcmp(p->name, S.project.name) != 0)) select_project(p, now);
}

static void picker_open(int64_t now)
{
    if (S.mode == RUN_RUNNING) {
        bump(STR_PAUSE_TO_PICK, now);
        return;
    }
    if (!cloud_toggl_configured()) {
        bump(STR_TOGGL_OFF, now);
        return;
    }
    cloud_refresh_projects();
    P.idx = 0;
    for (int i = 0; i < P.count; i++) {
        if (P.list[i].id == S.project.id) P.idx = i + 1;
    }
    P.pos = (float)P.idx;
    P.mode = PICK_PROJECTS;
    P.open = true;
    P.until = now + PICKER_IDLE_US;
    haptics_play(HAP_TAP);
}

// The Toggl button: recent entries, fetched fresh every time.
static void entries_open(int64_t now)
{
    cloud_fetch_entries();
    P.mode = PICK_ENTRIES;
    P.entries_state = ENTRIES_LOADING;
    P.nentries = 0;
    P.idx = 0;
    P.pos = 0;
    P.touched = false;
    P.open = true;
    P.until = now + PICKER_IDLE_US;
    haptics_play(HAP_TAP);
}

static void entries_update(int64_t now)
{
    if (!P.open || P.mode != PICK_ENTRIES || P.entries_state != ENTRIES_LOADING) return;
    const entries_state_t st = cloud_entries(P.entries, MAX_ENTRIES, &P.nentries);
    if (st == ENTRIES_LOADING) return;
    P.entries_state = st;
    P.until = now + PICKER_IDLE_US;
    if (st == ENTRIES_FAILED) haptics_play(HAP_BUMP);
    if (st == ENTRIES_OK && P.nentries && !P.touched) P.idx = 1; // the newest entry; "No task" is one click up
}

static void choose_entry(const toggl_entry_t *e, int64_t now)
{
    strlcpy(S.task, e->label, sizeof S.task);
    S.task_id = e->task_id;
    const project_t *p = e->project.id ? find_project(e->project.id) : NULL;
    select_project(p ? p : (e->project.id ? &e->project : NULL), now);
    ESP_LOGI(TAG, "task from Toggl: \"%s\"", S.task);
}

static void picker_close(bool choose, int64_t now)
{
    if (choose && P.mode == PICK_PROJECTS) {
        const project_t *p = P.idx ? &P.list[P.idx - 1] : NULL;
        if ((p ? p->id : 0) != S.project.id) S.task_id = 0; // a Toggl task belongs to its project
        select_project(p, now);
    } else if (choose && P.idx == 0) { // "No task": clear the task and the project that came with it
        S.task[0] = 0;
        S.task_id = 0;
        select_project(NULL, now);
        ESP_LOGI(TAG, "task cleared");
    } else if (choose && P.idx <= P.nentries) {
        choose_entry(&P.entries[P.idx - 1], now);
    }
    P.open = false;
    s_force_render = true;
    haptics_play(HAP_TAP);
}

// ---- Voice ----------------------------------------------------------------------

// The buttons show while the timer is stopped (and not while the profile dots are up).
static bool buttons_visible(int64_t now)
{
    return !P.open && (S.mode == RUN_IDLE || S.mode == RUN_PAUSED) && now >= S.dots_until;
}
static bool mic_visible(int64_t now) { return cloud_voice_configured() && buttons_visible(now); }
static bool toggl_visible(int64_t now) { return cloud_toggl_configured() && buttons_visible(now); }
// Squared distance from a touch to a button center.
static int dist2(int x, int y, int cy)
{
    const int dx = x - 180, dy = y - cy;
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
    if (now - S.listen_us < MIN_LISTEN_US || !cloud_transcribe(pcm, n)) {
        voice_fail(STR_NOT_HEARD, now);
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
        if (p && p->id != S.project.id) select_project(p, now);
        if (S.mode == RUN_RUNNING) cloud_session_rename(S.task, S.project.id, S.task_id);
        break;
    }
    case STT_NO_SPEECH: voice_fail(STR_NOT_HEARD, now); break;
    case STT_NO_WIFI: voice_fail(STR_NO_WIFI, now); break;
    default: voice_fail(STR_STT_FAILED, now); break;
    }
}

// ---- Input ----------------------------------------------------------------------

static void turn(int dir, int64_t now)
{
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
    if (S.mode == RUN_DONE) {
        bump(-1, now);
        return;
    }
    if (S.mode == RUN_RUNNING) {
        bump(STR_PAUSE_TO_ADJUST, now);
        return;
    } // the time is locked while counting down
    if (S.mode == RUN_IDLE) {
        const int n = S.set_min + dir;
        if (n < 1 || n > MAX_MIN) {
            bump(n < 1 ? STR_MIN_1 : STR_MAX_60, now);
            return;
        }
        S.set_min = n;
        S.remaining_us = n * MIN_US;
        mark_dirty(now);
    } else { // paused: step whole minutes; the seconds reset to zero
        const int64_t m = S.remaining_us / MIN_US + dir;
        if (m < 1 || m > MAX_MIN) {
            bump(m < 1 ? STR_MIN_1 : STR_MAX_60, now);
            return;
        }
        S.remaining_us = m * MIN_US;
    }
    haptics_play(HAP_CLICK);
}

// Start, pause, resume; dismiss "time's up".
static void run_toggle(int64_t now)
{
    if (S.mode == RUN_IDLE || S.mode == RUN_PAUSED) {
        S.mode = RUN_RUNNING;
        cloud_session_start(S.task, S.project.id, S.task_id, now);
    } else if (S.mode == RUN_RUNNING) {
        S.mode = RUN_PAUSED;
        cloud_session_stop(now);
    } else {
        reset_to_set();
    }
    haptics_play(HAP_TAP);
}

static void tap(int64_t now)
{
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
    const int d_mic = mic_visible(now) ? dist2(x, y, RENDER_MIC_CY) : INT32_MAX;
    const int d_top = toggl_visible(now) ? dist2(x, y, RENDER_TOP_CY) : INT32_MAX;
    if (d_mic < BTN_HIT_R * BTN_HIT_R) {
        if (S.voice == VOICE_IDLE) listen_start(now);
        return;
    }
    if (d_top < BTN_HIT_R * BTN_HIT_R) {
        if (S.voice == VOICE_IDLE) entries_open(now);
        return;
    }
    if (d_mic < BTN_DEAD_R * BTN_DEAD_R || d_top < BTN_DEAD_R * BTN_DEAD_R) {
        haptics_play(HAP_BUMP); // a near miss: don't start the timer
        return;
    }
    run_toggle(now);
}

static void hold(int64_t now)
{
    if (P.open || S.voice == VOICE_LISTENING) return;
    if (S.mode == RUN_IDLE) {
        bump(-1, now);
        return;
    }
    if (S.mode == RUN_RUNNING) cloud_session_stop(now);
    reset_to_set();
    show_hint(STR_RESET, now);
    haptics_play(HAP_DOUBLE);
}

static void swipe(int dir, int64_t now)
{
    if (P.open || S.voice == VOICE_LISTENING) return;
    if (S.mode == RUN_RUNNING) {
        bump(STR_PAUSE_TO_CHANGE, now);
        return;
    }
    S.palette = (S.palette + dir + PALETTE_COUNT) % PALETTE_COUNT;
    show_hint(STR_PAL_FIRST + S.palette, now);
    if (now >= S.dots_until) S.dots_start = now;
    S.dots_until = now + DOTS_US;
    mark_dirty(now);
    haptics_play(HAP_TAP);
}

static void swipe_vertical(bool down, int64_t now)
{
    if (S.voice == VOICE_LISTENING) return;
    if (P.open) {
        if (!down) picker_close(false, now);
        return;
    }
    if (down) picker_open(now);
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

// Debug: one line with everything a test needs to know ("STATE {json}").
static void state_dump(void)
{
    static const char *const MODES[] = {"idle", "running", "paused", "done"};
    static const char *const VOICES[] = {"idle", "listening", "waiting"};
    static const char *const ENTRIES[] = {"idle", "loading", "ok", "failed"};
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "mode", MODES[S.mode]);
    cJSON_AddNumberToObject(j, "remaining_s", (double)((S.remaining_us + 999999) / 1000000));
    cJSON_AddNumberToObject(j, "set_min", S.set_min);
    cJSON_AddNumberToObject(j, "palette", S.palette);
    cJSON_AddStringToObject(j, "voice", VOICES[S.voice]);
    cJSON_AddStringToObject(j, "task", S.task);
    cJSON_AddNumberToObject(j, "task_id", (double)S.task_id);
    cJSON_AddStringToObject(j, "project", S.project.id ? S.project.name : "");
    cJSON_AddNumberToObject(j, "project_id", (double)S.project.id);
    cJSON_AddStringToObject(j, "list", !P.open ? "closed" : P.mode == PICK_PROJECTS ? "projects" : "entries");
    cJSON_AddNumberToObject(j, "list_idx", P.idx);
    cJSON_AddNumberToObject(j, "projects", P.count);
    cJSON_AddNumberToObject(j, "entries", P.nentries);
    cJSON_AddStringToObject(j, "entries_state", ENTRIES[P.entries_state]);
    cJSON_AddBoolToObject(j, "wifi", net_connected());
    cJSON_AddBoolToObject(j, "toggl", cloud_toggl_ready());
    cJSON_AddBoolToObject(j, "voice_configured", cloud_voice_configured());
    const int64_t now = esp_timer_get_time();
    cJSON_AddBoolToObject(j, "mic_shown", mic_visible(now) || S.voice != VOICE_IDLE);
    cJSON_AddBoolToObject(j, "entries_shown", toggl_visible(now));
    cJSON_AddBoolToObject(j, "pulsing", S.mode == RUN_DONE || (S.mode == RUN_RUNNING && S.remaining_us <= MIN_US));
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
    static const char *const MODES[] = {"idle", "running", "paused", "done"};
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "uptime_s", (double)(now / 1000000));
    cJSON_AddStringToObject(j, "mode", MODES[S.mode]);
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
//   #project ID       select a project by Toggl id (0 = none)
//   #set N            timer length in minutes (while reset)
//   #wifi on|off      drop / restore Wi-Fi (offline behavior)
//   #telemetry on|off TELEMETRY lines every 5 s (saved)
//   #list             print the project and entry lists
//   #reboot           restart
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
    } else if (!strcmp(cmd, "project")) {
        const int64_t id = strtoll(arg, NULL, 10);
        const project_t *p = find_project(id);
        ok = id == 0 || p;
        if (ok) select_project(id ? p : NULL, now);
    } else if (!strcmp(cmd, "set")) {
        const int n = atoi(arg);
        ok = S.mode == RUN_IDLE && n >= 1 && n <= MAX_MIN;
        if (ok) {
            S.set_min = n;
            reset_to_set();
            mark_dirty(now);
        }
    } else if (!strcmp(cmd, "wifi")) {
        net_debug_enable(strcmp(arg, "off") != 0);
    } else if (!strcmp(cmd, "telemetry")) {
        S.telemetry = strcmp(arg, "off") != 0;
        mark_dirty(now);
    } else if (!strcmp(cmd, "list")) {
        list_dump();
    } else if (!strcmp(cmd, "reboot")) {
        printf("CMD ok reboot\n");
        settings_t st = {.set_min = S.set_min, .palette = S.palette, .task_id = S.task_id, .project = S.project,
                         .telemetry = S.telemetry};
        strlcpy(st.task, S.task, sizeof st.task);
        settings_save(&st);
        esp_restart();
    } else {
        ok = false;
    }
    char reply[160];
    snprintf(reply, sizeof reply, "%s %s %s", ok ? "ok" : "error", cmd, arg);
    emit("CMD ", reply);
}

// Debug: single-letter commands over the USB serial port act like the buttons,
// so the voice and Toggl flows can be tested from a computer.
//   m = tap the mic, e = tap the Toggl entries button, p = swipe down (projects),
//   t = tap (choose / stop listening), u = swipe up, s = start/pause the timer, r = reset,
//   + / - = turn the knob, < / > = swipe left / right, d = print the state, c = capture the screen
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
            if (mic_visible(now) && S.voice == VOICE_IDLE) listen_start(now);
            break;
        case 'e':
            if (toggl_visible(now) && S.voice == VOICE_IDLE) entries_open(now);
            break;
        case 'p': swipe_vertical(true, now); break;
        case 's':
            if (!P.open && S.voice == VOICE_IDLE) run_toggle(now);
            break;
        case 'r': hold(now); break;
        case '+': turn(+1, now); break;
        case '-': turn(-1, now); break;
        case '<': swipe(+1, now); break;
        case '>': swipe(-1, now); break;
        case 'd': state_dump(); break;
        case 'c': s_want_capture = true; break;
        case 'u': swipe_vertical(false, now); break;
        case 't':
            if (P.open) picker_close(true, now);
            else if (S.voice == VOICE_LISTENING) listen_stop(now);
            break;
        default: break;
        }
    }
}

static bool scene_differs(const scene_t *a, const scene_t *b)
{
    return a->palette != b->palette || a->status != b->status || memcmp(a->digits, b->digits, 4) != 0 ||
           rgb_to_565(a->live_rgb) != rgb_to_565(b->live_rgb) || fabsf(a->slice_deg - b->slice_deg) > 0.04f ||
           fabsf(a->digit_alpha - b->digit_alpha) > 0.01f || fabsf(a->colon_alpha - b->colon_alpha) > 0.01f ||
           fabsf(a->digit_scale - b->digit_scale) > 0.002f || fabsf(a->glow - b->glow) > 0.01f ||
           fabsf(a->status_alpha - b->status_alpha) > 0.01f || fabsf(a->dots_alpha - b->dots_alpha) > 0.01f ||
           fabsf(a->hold_progress - b->hold_progress) > 0.01f || fabsf(a->task_alpha - b->task_alpha) > 0.01f ||
           strcmp(a->task, b->task) != 0 || a->task_scroll != b->task_scroll || strcmp(a->project, b->project) != 0 ||
           fabsf(a->project_alpha - b->project_alpha) > 0.01f || a->rim_on != b->rim_on || a->rim_rgb != b->rim_rgb ||
           fabsf(a->mic_alpha - b->mic_alpha) > 0.01f || fabsf(a->mic_ring_alpha - b->mic_ring_alpha) > 0.02f ||
           fabsf(a->top_alpha - b->top_alpha) > 0.01f || a->wifi_rim != b->wifi_rim ||
           fabsf(a->wifi_alpha - b->wifi_alpha) > 0.02f || a->wave_on != b->wave_on || memcmp(a->wave, b->wave, 21) != 0;
}

#define POMODORO_BENCH 0 // set to 1 to log render timings for a few test scenes at boot
#define MIC_SELFTEST 0   // set to 1 to record 2 s at boot and log the microphone levels

#if POMODORO_BENCH
static void bench_case(const char *name, scene_t sc, int n, bool sweep, bool pulse)
{
    render_stats_t st;
    render_take_stats(&st);
    for (int i = 0; i < n; i++) {
        if (sweep) sc.slice_deg = 20.0f + 320.0f * i / n;
        if (pulse) {
            const float f = (float)i / n;
            sc.digit_scale = 1 + 0.07f * f;
            sc.glow = 0.85f * f;
        }
        if (!sweep && !pulse) render_invalidate(); // worst case: full redraw every frame
        render_frame(&sc);
    }
    display_wait_idle();
    render_take_stats(&st);
    ESP_LOGI(TAG, "bench %-8s %.1f ms/frame, %.1f bands (text %.1f, DMA wait %.1f, pixels %.1f)", name,
             st.total_us / 1000.0 / st.frames, (double)st.bands / st.frames, st.text_us / 1000.0 / st.frames,
             st.wait_us / 1000.0 / st.frames, (st.total_us - st.text_us - st.wait_us) / 1000.0 / st.frames);
}

static void bench(int palette)
{
    scene_t sc = {.palette = palette, .live_rgb = PALETTES[palette].slice, .slice_deg = 150, .digits = {'2', '5', '0', '0'},
                  .digit_alpha = 1, .colon_alpha = 1, .digit_scale = 1, .status = STR_TAP_TO_START, .status_alpha = 1};
    bench_case("static", sc, 10, false, false);
    bench_case("sweep", sc, 20, true, false);
    sc.status = -1;
    bench_case("pulse", sc, 20, false, true);
}
#endif

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
        settings_t st = {.set_min = S.set_min, .palette = S.palette};
        settings_load(&st);
        S.set_min = st.set_min;
        S.palette = st.palette;
        strlcpy(S.task, st.task, sizeof S.task);
        S.task_id = st.task_id;
        S.project = st.project;
        S.telemetry = st.telemetry;
    }
    P.entries = heap_caps_calloc(MAX_ENTRIES, sizeof(toggl_entry_t), MALLOC_CAP_SPIRAM);
    if (S.set_min < 1 || S.set_min > MAX_MIN) S.set_min = 25;
    if (S.palette < 0 || S.palette >= PALETTE_COUNT) S.palette = 0;
    reset_to_set();
    ESP_LOGI(TAG, "starting: %d min, profile %d, task \"%s\", project %s", S.set_min, S.palette, S.task,
             S.project.id ? S.project.name : "none");

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
#if POMODORO_BENCH
    display_set_backlight(255);
    bench(S.palette);
#endif
    vTaskDelay(pdMS_TO_TICKS(500));
    input_take_knob_steps(); // drop any power-up glitch on the knob lines

    lab_t base = rgb_to_oklab(PALETTES[S.palette].slice);
    float shown = S.remaining_us / (float)MIN_US;
    int64_t last = esp_timer_get_time(), pulse_start = -1, stats_since = last, render_us = 0;
    int frames = 0;
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
    int pk_drawn_idx = -1, pk_drawn_count = -1;

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
        case TOUCH_SWIPE_DOWN: swipe_vertical(true, now); break;
        case TOUCH_SWIPE_UP: swipe_vertical(false, now); break;
        default: break;
        }
        if (DEBUG_INTERFACE) {
            serial_commands(now);
            telemetry(now);
        }
        voice_update(now);
        sync_projects(now);
        entries_update(now);
        if (P.open && now > P.until) picker_close(false, now);

        // Clock
        if (S.mode == RUN_RUNNING) {
            S.remaining_us -= dt;
            if (S.remaining_us <= 0) finish(now);
        }
        if (S.dirty_since && now - S.dirty_since > SAVE_DELAY_US) {
            settings_t st = {.set_min = S.set_min, .palette = S.palette, .task_id = S.task_id, .project = S.project,
                             .telemetry = S.telemetry};
            strlcpy(st.task, S.task, sizeof st.task);
            settings_save(&st);
            S.dirty_since = 0;
        }

        // Project list: drawn instead of the dial while open.
        if (P.open) {
            P.pos += (P.idx - P.pos) * (1 - expf(-dt_ms / 70));
            if (fabsf(P.idx - P.pos) < 0.002f) P.pos = (float)P.idx;
            const int content = P.mode == PICK_PROJECTS ? P.count : P.nentries * 4 + P.entries_state;
            if (P.pos != pk_drawn_pos || P.idx != pk_drawn_idx || content != pk_drawn_count) {
                if (P.mode == PICK_PROJECTS) {
                    pv.count = P.count + 1;
                    pv.names[0] = "No project";
                    pv.hollow[0] = true;
                    for (int i = 0; i < P.count; i++) {
                        pv.names[i + 1] = P.list[i].name;
                        pv.colors[i + 1] = P.list[i].color;
                        pv.hollow[i + 1] = false;
                    }
                    pv.hint = P.count ? STR_TAP_TO_CHOOSE : STR_NO_PROJECTS;
                } else {
                    pv.count = P.nentries + 1;
                    pv.names[0] = "No task";
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
            }
            if (s_want_capture) {
                s_want_capture = false;
                capture_send(NULL, &pv);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        pk_drawn_pos = -1;

        // Animated values: the hand glides to knob changes; profile swaps fade the slice color.
        const float target = S.remaining_us / (float)MIN_US;
        shown += (target - shown) * (1 - expf(-dt_ms / 60));
        if (fabsf(target - shown) < 0.0005f) shown = target;
        base = lab_mix(base, rgb_to_oklab(PALETTES[S.palette].slice), 1 - expf(-dt_ms / 120));

        scene_t sc = {.palette = S.palette, .digit_scale = 1, .status_alpha = 1};
        sc.live_rgb = oklab_to_rgb(warm_color(base, shown));
        sc.slice_deg = fminf(360, fmaxf(0, shown * 6));

        int secs = (int)((S.remaining_us + 999999) / 1000000);
        if (secs < 0) secs = 0;
        const int mm = secs / 60, ss = secs % 60;
        sc.digits[0] = (char)('0' + mm / 10);
        sc.digits[1] = (char)('0' + mm % 10);
        sc.digits[2] = (char)('0' + ss / 10);
        sc.digits[3] = (char)('0' + ss % 10);

        const bool pulsing = S.mode == RUN_DONE || (S.mode == RUN_RUNNING && S.remaining_us <= MIN_US);
        sc.digit_alpha = S.mode == RUN_PAUSED ? 0.55f : 1;
        sc.colon_alpha = S.mode == RUN_RUNNING && !pulsing && (now / 1000) % 1000 >= 500 ? 0.3f : 1;
        if (pulsing) {
            // Heartbeat: quick swell with a glow, then settle, once a second.
            if (pulse_start < 0) pulse_start = now;
            const float p = fmodf((now - pulse_start) / 1e6f, 1.0f);
            const float f = p < 0.18f ? sinf(p / 0.18f * (float)M_PI / 2)
                                      : 0.5f + 0.5f * cosf((p - 0.18f) / 0.82f * (float)M_PI);
            sc.digit_scale = 1 + 0.07f * f;
            sc.glow = 0.85f * f;
        } else {
            pulse_start = -1;
        }

        const bool hinting = S.hint >= 0 && now < S.hint_until;
        const bool voice_busy = S.voice == VOICE_LISTENING || S.voice == VOICE_WAITING;
        sc.status = hinting                       ? S.hint
                    : S.voice == VOICE_LISTENING ? -1 // the waveform shows instead
                    : S.voice == VOICE_WAITING   ? STR_TRANSCRIBING
                    : S.mode == RUN_IDLE         ? STR_TAP_TO_START
                    : S.mode == RUN_PAUSED ? STR_PAUSED
                    : S.mode == RUN_DONE   ? STR_TIMES_UP
                                           : -1;
        if ((S.mode == RUN_PAUSED || voice_busy) && !hinting) {
            const float b = sinf((float)M_PI * fmodf(now / 1e6f, 2.0f) / 2.0f);
            sc.status_alpha = 1 - 0.65f * b * b;
        }
        if (now < S.dots_until) {
            sc.dots_alpha = fminf(1, (now - S.dots_start) / FADE_US) * fminf(1, (S.dots_until - now) / FADE_US);
        }
        // Above the digits: the spoken task, with the project in small type under it.
        strlcpy(sc.task, S.task, sizeof sc.task);
        strlcpy(sc.project, S.project.id ? S.project.name : "", sizeof sc.project);
        sc.task_alpha = S.mode == RUN_PAUSED ? 0.55f : 1;
        sc.project_alpha = sc.task_alpha * 0.65f;
        // A task too wide for the screen scrolls: rest at the start, then glide through in a loop.
        if (strcmp(marquee_task, S.task) != 0) {
            strlcpy(marquee_task, S.task, sizeof marquee_task);
            marquee_since = now;
            marquee_w = S.task[0] ? render_task_width(S.task) : 0;
        }
        sc.task_scroll = -1;
        if (marquee_w > TASK_MAX_W) {
            const int64_t pass_us = (int64_t)(marquee_w + TASK_SCROLL_GAP) * 1000000 / MARQUEE_PX_PER_S;
            const int64_t t = (now - marquee_since) % (MARQUEE_PAUSE_US + pass_us);
            sc.task_scroll = t < MARQUEE_PAUSE_US ? 0 : (int)((t - MARQUEE_PAUSE_US) * MARQUEE_PX_PER_S / 1000000);
        }
        sc.rim_on = S.project.id != 0;
        sc.rim_rgb = S.project.color;
        // Mic: dim at rest, solid while listening, its ring following the voice level.
        mic_lvl += (mic_level() - mic_lvl) * (1 - expf(-dt_ms / 80));
        const bool mic_on = mic_visible(now) || S.voice != VOICE_IDLE;
        sc.mic_alpha = !mic_on ? 0 : S.voice == VOICE_LISTENING ? 1 : 0.75f;
        sc.mic_ring_alpha = !mic_on ? 0 : S.voice == VOICE_LISTENING ? 0.45f + 0.55f * mic_lvl : 0.45f;
        const bool top_on = toggl_visible(now) || (cloud_toggl_configured() && S.voice != VOICE_IDLE);
        sc.top_alpha = !top_on ? 0 : S.voice == VOICE_IDLE ? 1 : 0.4f;
        // Waveform while listening: the last ~1.3 s of mic level, one bar per 60 ms.
        if (S.voice == VOICE_LISTENING) {
            if (now - wave_t >= 60000) {
                wave_t = now;
                memmove(wave_lv, wave_lv + 1, sizeof wave_lv - sizeof wave_lv[0]);
                wave_lv[20] = sqrtf(mic_level());
            }
            sc.wave_on = true;
            for (int i = 0; i < 21; i++) sc.wave[i] = (uint8_t)(3 + 21 * wave_lv[i]);
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
            const int64_t t0 = esp_timer_get_time();
            render_frame(&sc);
            render_us += esp_timer_get_time() - t0;
            frames++;
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
                         st.text_us / 1000.0 / st.frames,
                         st.wait_us / 1000.0 / st.frames, (st.total_us - st.text_us - st.wait_us) / 1000.0 / st.frames);
            }
            frames = 0;
            render_us = 0;
            stats_since = now;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
