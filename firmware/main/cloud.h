#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "project.h"

// Network work, done on a background task so the screen never waits:
// speech-to-text (OpenAI) and time tracking (Toggl 2.0).

typedef enum {
    STT_PENDING,
    STT_OK,
    STT_NO_SPEECH, // nothing that sounded like speech, or an empty transcript
    STT_NO_WIFI,
    STT_FAILED,
} stt_result_t;

void cloud_init(void);
bool cloud_voice_configured(void); // Wi-Fi and an OpenAI key are set in secrets.h
bool cloud_online(void);

// Transcribe a recording. The samples must stay untouched until the result arrives.
// False if voice is off or the request couldn't be queued.
bool cloud_transcribe(int16_t *pcm, size_t n);
stt_result_t cloud_take_transcript(char *out, size_t cap); // STT_PENDING until finished

// Toggl: one time entry per stretch of running time (pauses are not logged). Entries
// start right away; a stretch stopped within 10 s is deleted (accidental taps).
// Times are esp_timer microseconds; they are converted to UTC when sent.
// project_id / task_id 0 = none (task_id links the entry to a Toggl task).
void cloud_session_start(const char *description, int64_t project_id, int64_t task_id, int64_t at_us);
void cloud_session_stop(int64_t at_us); // also starting another stretch stops the running one
void cloud_session_update(const char *description, int64_t project_id, int64_t task_id); // the running stretch
void cloud_session_shift(int64_t start_us); // the running stretch now started at start_us (knob adjust)
void cloud_tick(void); // call every UI loop: posts knob adjustments once the knob rests
bool cloud_take_dropped(void); // true once after Toggl turned a stretch or change down for good

// Swipe down: one check of the timer running in Toggl. If another timer runs, the
// knob adopts it (its details come back in toggl_running_t); if the knob's own
// entry was stopped elsewhere, that comes back as SYNC_STOPPED.
typedef enum { SYNC_IDLE, SYNC_PENDING, SYNC_IN_SYNC, SYNC_ADOPTED, SYNC_STOPPED, SYNC_FAILED } sync_result_t;
typedef struct {
    int64_t id;
    char desc[96];
    int64_t project_id, task_id;
    int64_t start_us; // esp_timer time
} toggl_running_t;
bool cloud_check_running(bool knob_running); // false if it couldn't be queued
sync_result_t cloud_take_check(toggl_running_t *out); // SYNC_PENDING until done; a result is returned once

// Debug (tests): start or stop a timer in Toggl directly, as another device would.
void cloud_debug_start_elsewhere(const char *description);
void cloud_debug_stop_elsewhere(void);
int64_t cloud_running_entry(void); // Toggl id of the knob's running entry, 0 = none or not created yet

// The knob caches the recent entries and the projects in NVS, so both lists open at once,
// also offline, and survive a restart. Toggl is asked for them only when the cache is empty
// (first boot) or when the user picks "Sync from Toggl" in a list.
//
// Recent entries: newest first, one per task/project pair (the running one too), plus the
// tasks the knob itself starts or renames (cloud_remember_entry).
#define MAX_ENTRIES 20
typedef struct {
    char label[96];    // the entry's description, or its Toggl task's name
    int64_t task_id;   // 0 = no Toggl task
    project_t project; // id 0 = no project
} toggl_entry_t;
// The state of a cached list's last sync from Toggl (recent entries or projects).
typedef enum { ENTRIES_IDLE, ENTRIES_LOADING, ENTRIES_OK, ENTRIES_FAILED } entries_state_t;
bool cloud_fetch_entries(void); // sync the recent entries from Toggl; false if it couldn't be queued
entries_state_t cloud_entries(toggl_entry_t *out, int max, int *n); // copies the cached list; returns the sync state
void cloud_remember_entry(const char *label, int64_t task_id, const project_t *project); // to the top, no request

// Active Toggl projects in the default workspace, sorted by name (cached, see above).
// `version` changes whenever the list is loaded (0 = never loaded).
bool cloud_toggl_configured(void);
bool cloud_toggl_ready(void); // connected to the workspace
void cloud_debug_fields(void *json_object); // adds Toggl/upload counters to a cJSON object (telemetry)
int cloud_projects(project_t *out, int max, uint32_t *version);
uint32_t cloud_projects_version(void);
bool cloud_sync_projects(void);                    // reload from Toggl; false if it couldn't be queued
entries_state_t cloud_projects_state(void);        // the state of the last cloud_sync_projects()
int64_t cloud_match_project(const char *text);     // a project named in `text` (whole words), or 0
