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
bool cloud_transcribe(int16_t *pcm, size_t n);
stt_result_t cloud_take_transcript(char *out, size_t cap); // STT_PENDING until finished

// Toggl: one time entry per stretch of running time (pauses are not logged).
// Times are esp_timer microseconds; they are converted to UTC when sent.
// project_id / task_id 0 = none (task_id links the entry to a Toggl task).
void cloud_session_start(const char *description, int64_t project_id, int64_t task_id, int64_t at_us);
void cloud_session_stop(int64_t at_us);
void cloud_session_rename(const char *description, int64_t project_id, int64_t task_id); // the running stretch

// Recent Toggl entries (finished ones, newest first, one per task/project pair),
// fetched fresh on every cloud_fetch_entries(); at most 50 entries are requested.
#define MAX_ENTRIES 50
typedef struct {
    char label[96];    // the entry's description, or its Toggl task's name
    int64_t task_id;   // 0 = no Toggl task
    project_t project; // id 0 = no project
} toggl_entry_t;
typedef enum { ENTRIES_IDLE, ENTRIES_LOADING, ENTRIES_OK, ENTRIES_FAILED } entries_state_t;
void cloud_fetch_entries(void);
entries_state_t cloud_entries(toggl_entry_t *out, int max, int *n); // copies the list once it's ENTRIES_OK

// Active Toggl projects in the default workspace, sorted by name. `version`
// changes whenever the list is reloaded (0 = never loaded).
bool cloud_toggl_configured(void);
bool cloud_toggl_ready(void); // connected to the workspace
void cloud_debug_fields(void *json_object); // adds Toggl/upload counters to a cJSON object (telemetry)
int cloud_projects(project_t *out, int max, uint32_t *version);
uint32_t cloud_projects_version(void);
void cloud_refresh_projects(void);                 // reload if the list is more than 2 minutes old
int64_t cloud_match_project(const char *text);     // a project named in `text` (whole words), or 0
