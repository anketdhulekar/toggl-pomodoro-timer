#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Onboard PDM microphone (MSM261D4030H1CPM on GPIO45/46), 16 kHz mono.
#define MIC_RATE 16000
#define MIC_MAX_SECONDS 15

void mic_init(void);
bool mic_start(void);           // start capturing in the background; false if busy
bool mic_full(void);            // reached MIC_MAX_SECONDS
size_t mic_stop(int16_t **pcm); // stop and return the samples (valid until the next mic_start)
void mic_cancel(void);          // stop and discard

// While capturing: loudness above the room noise (0..1, for the UI), and whether
// the speaker has finished (speech followed by a pause, or nothing said for a while).
float mic_level(void);
bool mic_speech_done(void);

// Clean up a recording for speech recognition: drop the start-up click, remove
// DC, trim silence at both ends and normalize the level. Returns false if it
// doesn't sound like anyone spoke. Updates *n to the trimmed length and *pcm
// to the first kept sample.
bool mic_prepare(int16_t **pcm, size_t *n);
