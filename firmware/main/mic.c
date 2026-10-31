#include "mic.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2s_pdm.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "mic";

#define PIN_MIC_CLK 45
#define PIN_MIC_DATA 46
#define MAX_SAMPLES (MIC_RATE * MIC_MAX_SECONDS)
#define CHUNK 256 // samples per read (16 ms), which bounds how long a stop waits

static i2s_chan_handle_t s_rx;
static int16_t *s_buf;
static volatile size_t s_len;
static volatile bool s_run;  // capture requested
static volatile bool s_busy; // capture loop active
static SemaphoreHandle_t s_done;
static TaskHandle_t s_task;

// Live speech tracking, updated per chunk by the capture task.
#define END_PAUSE_US 1300000  // this much quiet after speech ends the recording
#define GIVE_UP_US 6000000    // nothing said for this long: stop anyway
#define WARMUP_CHUNKS 3       // the PDM filter settles over the first ~50 ms
static volatile float s_level;
static volatile bool s_done_talking;
static float s_floor; // room noise estimate (mean absolute deviation per chunk)
static uint32_t s_peak_lv;
static int s_loud_run;     // consecutive loud chunks
static int64_t s_heard_us; // speech the live tracker heard in the last capture
static float s_voice_lv;   // smoothed loudness of the speech heard so far
static int64_t s_start_us, s_last_voice_us, s_voiced_us;
static int s_chunks;

static void track(const int16_t *x, size_t n)
{
    if (!n) return;
    int32_t sum = 0;
    for (size_t i = 0; i < n; i++) sum += x[i];
    const int32_t dc = sum / (int32_t)n;
    uint32_t dev = 0;
    for (size_t i = 0; i < n; i++) dev += abs(x[i] - dc);
    const uint32_t lv = dev / n;
    const int64_t now = esp_timer_get_time();
    if (++s_chunks <= WARMUP_CHUNKS) return;
    if (s_chunks == WARMUP_CHUNKS + 1 || lv < s_floor) s_floor = (float)lv; // room noise: follows quiet quickly...
    else s_floor += (lv - s_floor) / 200.0f;                            // ...and rises slowly (~3 s)
    if (lv > s_peak_lv) s_peak_lv = lv;
    // Speech is loud for a while; a click or bump is one chunk. Once someone has
    // spoken, sounds much quieter than their voice don't count as more speech.
    float thresh = s_floor * 2.5f + 4.0f;
    if (s_voiced_us >= 150000 && s_voice_lv * 0.25f > thresh) thresh = s_voice_lv * 0.25f;
    s_loud_run = lv > thresh ? s_loud_run + 1 : 0;
    if (s_loud_run >= 3) { // ~50 ms of sustained sound
        s_last_voice_us = now;
        s_voiced_us += (int64_t)n * 1000000 / MIC_RATE;
        s_voice_lv = s_voice_lv ? s_voice_lv + (lv - s_voice_lv) * 0.1f : (float)lv;
    }
    const float level = (lv - s_floor) / (s_floor * 8.0f + 40.0f);
    s_level = level < 0 ? 0 : (level > 1 ? 1 : level);
    const bool heard = s_voiced_us >= 150000;
    s_done_talking = (heard && now - s_last_voice_us > END_PAUSE_US) || (!heard && now - s_start_us > GIVE_UP_US);
}

static void mic_task(void *arg)
{
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        s_start_us = s_last_voice_us = esp_timer_get_time();
        s_voiced_us = 0;
        s_chunks = 0;
        s_floor = 0;
        s_peak_lv = 0;
        s_loud_run = 0;
        s_voice_lv = 0;
        s_level = 0;
        s_done_talking = false;
        i2s_channel_enable(s_rx);
        size_t len = 0;
        while (s_run && len < MAX_SAMPLES) {
            size_t want = MAX_SAMPLES - len, got = 0;
            if (want > CHUNK) want = CHUNK;
            i2s_channel_read(s_rx, s_buf + len, want * sizeof(int16_t), &got, 100);
            track(s_buf + len, got / sizeof(int16_t));
            len += got / sizeof(int16_t);
            s_len = len;
        }
        i2s_channel_disable(s_rx);
        s_heard_us = s_voiced_us;
        ESP_LOGI(TAG, "captured %.1f s: room noise %lu, loudest %lu, speech %.1f s", len / (float)MIC_RATE,
                 (unsigned long)s_floor, (unsigned long)s_peak_lv, s_voiced_us / 1e6);
        s_len = len;
        s_busy = false;
        xSemaphoreGive(s_done);
    }
}

void mic_init(void)
{
    s_buf = heap_caps_malloc(MAX_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_done = xSemaphoreCreateBinary();
    const i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    if (!s_buf || !s_done || i2s_new_channel(&chan, NULL, &s_rx) != ESP_OK) {
        ESP_LOGE(TAG, "no memory for the microphone");
        s_rx = NULL;
        return;
    }
    i2s_pdm_rx_config_t cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(MIC_RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {.clk = PIN_MIC_CLK, .din = PIN_MIC_DATA},
    };
    // 16x down-sampling runs the mic's clock at 2.048 MHz, the middle of its normal
    // range (the default 8x gives 1.024 MHz, right at the bottom edge).
    cfg.clk_cfg.dn_sample_mode = I2S_PDM_DSR_16S;
    ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(s_rx, &cfg));
    xTaskCreatePinnedToCore(mic_task, "mic", 3072, NULL, 6, &s_task, 1);
    ESP_LOGI(TAG, "PDM microphone ready (%d Hz, up to %d s)", MIC_RATE, MIC_MAX_SECONDS);
}

bool mic_start(void)
{
    if (!s_rx) return false;
    // Collect the end of a cancelled capture, or clear a stale signal.
    xSemaphoreTake(s_done, s_busy ? pdMS_TO_TICKS(150) : 0);
    if (s_busy) return false;
    s_len = 0;
    s_busy = true;
    s_run = true;
    xTaskNotifyGive(s_task);
    return true;
}

bool mic_full(void) { return s_busy && s_len >= MAX_SAMPLES; }

size_t mic_stop(int16_t **pcm)
{
    s_run = false;
    if (s_busy) xSemaphoreTake(s_done, pdMS_TO_TICKS(200));
    *pcm = s_buf;
    return s_len;
}

void mic_cancel(void) { s_run = false; }

float mic_level(void) { return s_busy ? s_level : 0; }

bool mic_speech_done(void) { return s_busy && s_done_talking; }

// ---- Clean-up ----------------------------------------------------------------

#define FRAME (MIC_RATE / 50) // 20 ms
#define SKIP (MIC_RATE / 20)  // first 50 ms: the PDM filter settling

static int cmp_u32(const void *a, const void *b)
{
    const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

bool mic_prepare(int16_t **pcm, size_t *n)
{
    if (*n <= SKIP + 10 * FRAME) return false;
    int16_t *s = *pcm + SKIP;
    const size_t len = *n - SKIP;

    // High-pass at 150 Hz (4th-order Butterworth, two biquads): removes DC and
    // mains hum / rumble, keeps speech. Coefficients from the RBJ cookbook.
    static const float Q[2] = {0.5412f, 1.3066f};
    const float w0 = 2.0f * (float)M_PI * 150.0f / MIC_RATE, cw = cosf(w0), sw = sinf(w0);
    float b[2][3], a[2][2], z[2][2] = {{0}};
    for (int k = 0; k < 2; k++) {
        const float alpha = sw / (2 * Q[k]), a0 = 1 + alpha;
        b[k][0] = (1 + cw) / 2 / a0;
        b[k][1] = -(1 + cw) / a0;
        b[k][2] = (1 + cw) / 2 / a0;
        a[k][0] = -2 * cw / a0;
        a[k][1] = (1 - alpha) / a0;
    }
    for (size_t i = 0; i < len; i++) {
        float v = s[i];
        for (int k = 0; k < 2; k++) { // transposed direct form II
            const float y = b[k][0] * v + z[k][0];
            z[k][0] = b[k][1] * v - a[k][0] * y + z[k][1];
            z[k][1] = b[k][2] * v - a[k][1] * y;
            v = y;
        }
        s[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
    }

    // Loudness per 20 ms frame (mean absolute value, cheaper than RMS and good enough).
    const size_t frames = len / FRAME;
    uint32_t *lv = malloc(frames * sizeof(uint32_t)), *sorted = malloc(frames * sizeof(uint32_t));
    if (!lv || !sorted) {
        free(lv);
        free(sorted);
        return true; // can't analyze; let the recognizer decide
    }
    uint32_t peak_lv = 0;
    for (size_t f = 0; f < frames; f++) {
        uint32_t sum = 0;
        for (int i = 0; i < FRAME; i++) sum += abs(s[f * FRAME + i]);
        lv[f] = sum / FRAME;
        if (lv[f] > peak_lv) peak_lv = lv[f];
    }
    memcpy(sorted, lv, frames * sizeof(uint32_t));
    qsort(sorted, frames, sizeof(uint32_t), cmp_u32);
    const uint32_t floor_lv = sorted[frames / 5] ? sorted[frames / 5] : 1; // 20th percentile = room noise
    free(sorted);
    uint32_t thresh = floor_lv * 3;
    if (thresh < 12) thresh = 12;

    size_t loud = 0, first = frames, last = 0;
    for (size_t f = 0; f < frames; f++) {
        if (lv[f] > thresh) {
            loud++;
            if (f < first) first = f;
            last = f;
        }
    }
    free(lv);
    ESP_LOGI(TAG, "%.2f s, noise %lu, peak %lu, %u loud frames", len / (float)MIC_RATE, (unsigned long)floor_lv,
             (unsigned long)peak_lv, (unsigned)loud);
    if (loud < 6 && s_heard_us < 300000) return false; // neither check heard speech
    if (loud < 6) { // the live tracker heard speech: keep it all
        first = 0;
        last = frames;
    }

    // Keep 250 ms around the loud part.
    const size_t pad = 250 / 20;
    first = first > pad ? first - pad : 0;
    last = last + pad + 1 < frames ? last + pad + 1 : frames;
    s += first * FRAME;
    const size_t kept = (last - first) * FRAME;

    // Normalize the peak to about -3 dBFS, at most 30x (+30 dB).
    int peak = 1;
    for (size_t i = 0; i < kept; i++) {
        const int a = abs(s[i]);
        if (a > peak) peak = a;
    }
    int gain256 = 23000 * 256 / peak;
    if (gain256 > 30 * 256) gain256 = 30 * 256;
    if (gain256 > 256) {
        for (size_t i = 0; i < kept; i++) {
            const int32_t v = (int32_t)s[i] * gain256 >> 8;
            s[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
        }
    }
    *pcm = s;
    *n = kept;
    return true;
}
