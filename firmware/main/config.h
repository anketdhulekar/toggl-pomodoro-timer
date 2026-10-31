// Credentials come from main/secrets.h (git-ignored, filled in by you), or the
// empty template when that file doesn't exist yet.
#pragma once

#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

// Optional: your Toggl organization id, if the knob can't look it up with the API key.
#ifndef TOGGL_ORGANIZATION_ID
#define TOGGL_ORGANIZATION_ID 0
#endif

#define HAVE_WIFI (sizeof(WIFI_SSID) > 1)
#define HAVE_OPENAI (HAVE_WIFI && sizeof(OPENAI_API_KEY) > 1)
#define HAVE_TOGGL (HAVE_WIFI && sizeof(TOGGL_API_TOKEN) > 1)
