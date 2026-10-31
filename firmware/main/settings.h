#pragma once
#include <stdbool.h>

#include "project.h"

// Timer length (minutes), color profile, task and Toggl project, kept in NVS across power-offs.
#define SETTINGS_TASK_LEN 96

typedef struct {
    int set_min;
    int palette;
    char task[SETTINGS_TASK_LEN];
    int64_t task_id; // Toggl task, 0 = none
    project_t project;
    bool telemetry; // print TELEMETRY lines every 5 s
} settings_t;

void settings_init(void);
void settings_load(settings_t *s); // fields not in NVS keep their values
void settings_save(const settings_t *s);
