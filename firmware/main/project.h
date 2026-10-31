#pragma once
#include <stdint.h>

// A Toggl project as the knob uses it.
#define PROJECT_NAME_LEN 48
#define MAX_PROJECTS 40

typedef struct {
    int64_t id; // 0 = no project
    uint32_t color; // 0xRRGGBB, from Toggl
    char name[PROJECT_NAME_LEN];
} project_t;
