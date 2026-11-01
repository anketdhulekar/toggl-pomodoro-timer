#pragma once
#include <stdbool.h>

#include "project.h"

// Mode, color profile, Pomodoro length, task and Toggl project, kept in NVS across power-offs.
#define SETTINGS_TASK_LEN 96

typedef struct {
    int app;   // 0 = Tracking, 1 = Pomodoro
    int theme; // 0 = Mono, 1 = Inverted
    int set_min;
    char task[SETTINGS_TASK_LEN];
    int64_t task_id;   // Toggl task, 0 = none
    project_t project; // id 0 = none; left as it was if no project was ever saved
    bool telemetry;    // print TELEMETRY lines every 5 s
} settings_t;

void settings_init(void);
void settings_load(settings_t *s); // fields not in NVS keep their values
void settings_save(const settings_t *s);
