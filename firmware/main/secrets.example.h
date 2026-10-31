// Knob Pomodoro credentials: TEMPLATE. Copy it, then fill in the copy:
//
//     cp main/secrets.example.h main/secrets.h
//
// main/secrets.h is git-ignored. Never commit it, paste it into a chat, or share it.
// Leave a value empty ("") to turn that feature off: the timer itself needs none of them.
// After editing, rebuild and flash (see README.md).
#pragma once

// Wi-Fi network for voice and Toggl. Must be 2.4 GHz: the ESP32-S3 can't join 5 GHz-only networks.
#define WIFI_SSID ""
#define WIFI_PASSWORD ""

// OpenAI API key, used to turn what you say into the task name (platform.openai.com > API keys).
// Tip: give the knob its own key, on prepaid credit with auto-recharge off, so a leak is capped.
#define OPENAI_API_KEY ""

// Toggl 2.0 API key, starts with "toggl_sk_" (Toggl > Profile settings > API Token).
#define TOGGL_API_TOKEN ""

// Toggl 2.0 organization id: the first number in Toggl's web address,
// focus.toggl.com/<organization>/workspaces/<workspace>/...  (an API key can't look it up).
#define TOGGL_ORGANIZATION_ID 0
