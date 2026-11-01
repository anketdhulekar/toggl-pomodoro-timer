// Background network work: OpenAI speech-to-text and Toggl 2.0 time entries.
//
// One task owns all HTTPS traffic. The UI posts messages and polls results, so
// a slow network never stalls the screen.
//
// Toggl: each stretch of running time becomes one time entry. It starts in Toggl
// right away and is stopped when the timer pauses, resets, switches task or runs
// out; a stretch stopped within 10 s is deleted, so accidental taps leave nothing
// behind. If the network is down, the start and stop times are kept and sent
// later, so the entry still ends up with the right times. Toggl is only
// contacted on user actions (no polling: the free plan allows few requests).
#include "cloud.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>

#include "cJSON.h"
#include "config.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mic.h"
#include "net.h"
#include "render.h" // SCENE_TASK_LEN

static const char *TAG = "cloud";

#define STT_URL "https://api.openai.com/v1/audio/transcriptions"
#define STT_MODEL "gpt-transcribe"
#define TOGGL_API "https://focus.toggl.com/api"

#define MIN_SEG_US 10000000LL // stretches shorter than this are deleted
#define SETTLE_US 1500000LL   // changes to a running entry are sent once they stop coming (knob clicks)
#define SHIFT_REST_US 250000  // knob clicks are posted to the cloud task once the knob rests this long
#define MAX_SEGS 12
#define RESP_CAP (256 * 1024) // a page of entries (each with its task and project) is big
#define PROJECTS_MAX_AGE_US (120 * 1000000LL)
#define ENTRIES_FETCH 50 // newest entries requested per press of the Toggl button

typedef enum {
    MSG_TRANSCRIBE,
    MSG_START,
    MSG_STOP,
    MSG_UPDATE,
    MSG_SHIFT,
    MSG_CHECK,
    MSG_REFRESH,
    MSG_ENTRIES,
    MSG_DEBUG_START,
    MSG_DEBUG_STOP,
} msg_kind_t;

typedef struct {
    msg_kind_t kind;
    int64_t at_us;
    int64_t project_id, task_id;
    int16_t *pcm;
    size_t n;
    bool knob_running; // MSG_CHECK: the knob's Tracking timer runs
    char text[SCENE_TASK_LEN];
} msg_t;

typedef struct {
    char desc[SCENE_TASK_LEN];
    int64_t start_us, stop_us; // esp_timer time; stop 0 = still running
    int64_t id;                // Toggl time entry id, 0 = not created yet
    int64_t project_id, task_id;
    bool dirty_desc;           // description, project or task changed after creation
    bool dirty_start;          // start moved after creation (knob)
    bool adopted;              // started on another device (never deleted as an accidental start)
    int64_t changed_us;        // when (sent once SETTLE_US have passed without another change)
} seg_t;

#define QUEUE_LEN 32 // in PSRAM; knob clicks are merged before they're posted, so this never fills

static QueueHandle_t s_q;
static int s_dropped;        // stretches or changes Toggl turned down for good (atomics; the UI says so)
static int64_t s_rate_until; // Toggl answered 429: no new requests from swipe down or lists until then
static char *s_resp; // response buffer (PSRAM)

static stt_result_t s_stt = STT_PENDING; // written by the cloud task, read by the UI (atomics)
static char s_stt_text[SCENE_TASK_LEN];
static bool s_no_keywords; // the API turned down our keyword list once; don't send it again
static char s_head[2048];  // multipart preamble

static seg_t s_seg[MAX_SEGS];
static int s_nseg;
static int64_t s_oid, s_wid;       // Toggl organization and workspace
static bool s_toggl_off;           // token rejected
static int64_t s_next_try;         // esp_timer time of the next Toggl attempt after a failure
static int s_backoff_s = 5;
static char s_toggl_auth[96];
static char s_openai_auth[256];
static project_t s_proj[MAX_PROJECTS]; // written by the cloud task under s_proj_lock
static project_t *s_proj_tmp;          // PSRAM scratch for a fetch
static int s_nproj;
static uint32_t s_proj_version;
static int64_t s_proj_loaded_us;
static SemaphoreHandle_t s_proj_lock; // also guards the entries list
static toggl_entry_t *s_entries, *s_entries_tmp; // PSRAM
static int s_nentries;
static entries_state_t s_entries_state = ENTRIES_IDLE;
static sync_result_t s_check = SYNC_IDLE; // refresh result (atomics), details in s_check_info
static toggl_running_t s_check_info;

// ---- HTTPS ------------------------------------------------------------------

typedef struct {
    const void *p;
    size_t n;
} piece_t;

// Send a request whose body is the concatenation of `pieces`; returns the HTTP
// status (or -1 on a network error) and the response body in s_resp.
static int s_last_status;      // telemetry: last HTTP status
static int64_t s_last_http_ms; // telemetry: how long the last request took
static int s_http_errors;      // telemetry: network errors since boot

static int https(esp_http_client_method_t method, const char *url, const char *auth, const char *ctype,
                 const piece_t *pieces, int npieces, int timeout_ms)
{
    s_resp[0] = 0;
    const esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .addr_type = HTTP_ADDR_TYPE_INET, // IPv4 only: skip slow AAAA lookups
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
    };
    size_t total = 0;
    for (int i = 0; i < npieces; i++) total += pieces[i].n;

    int status = -1;
    const int64_t t0 = esp_timer_get_time();
    int64_t t_open = 0, t_sent = 0;
    // A connection that can't be opened is retried (nothing was sent yet, so it's
    // always safe): brief network hiccups shouldn't fail a request.
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt) {
            ESP_LOGW(TAG, "%.40s: couldn't connect, retrying", url + 8);
            vTaskDelay(pdMS_TO_TICKS(400 * attempt));
        }
        esp_http_client_handle_t c = esp_http_client_init(&cfg);
        if (!c) continue;
        if (auth) esp_http_client_set_header(c, "Authorization", auth);
        if (ctype) esp_http_client_set_header(c, "Content-Type", ctype);
        const bool opened = esp_http_client_open(c, (int)total) == ESP_OK;
        if (opened) {
            t_open = esp_timer_get_time();
            bool ok = true;
            for (int i = 0; ok && i < npieces; i++) {
                const char *p = pieces[i].p;
                size_t left = pieces[i].n;
                while (left > 0) {
                    const int w = esp_http_client_write(c, p, left > 8192 ? 8192 : (int)left);
                    if (w <= 0) {
                        ok = false;
                        break;
                    }
                    p += w;
                    left -= w;
                }
            }
            t_sent = esp_timer_get_time();
            if (ok && esp_http_client_fetch_headers(c) >= 0) {
                status = esp_http_client_get_status_code(c);
                const int got = esp_http_client_read_response(c, s_resp, RESP_CAP - 1);
                s_resp[got > 0 ? got : 0] = 0;
            }
        }
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        if (opened) break;
    }
    if (t_open) {
        ESP_LOGI(TAG, "%.40s: %u KB up, connect %lld ms, send %lld ms, reply %lld ms", url + 8, (unsigned)(total / 1024),
                 (t_open - t0) / 1000, (t_sent - t_open) / 1000, (esp_timer_get_time() - t_sent) / 1000);
    }
    s_last_status = status;
    s_last_http_ms = (esp_timer_get_time() - t0) / 1000;
    if (status < 0) s_http_errors++;
    return status;
}

static int https_json(esp_http_client_method_t method, const char *url, cJSON *body)
{
    char *txt = body ? cJSON_PrintUnformatted(body) : NULL;
    const piece_t piece = {txt, txt ? strlen(txt) : 0};
    const int st = https(method, url, s_toggl_auth, body ? "application/json" : NULL, &piece, txt ? 1 : 0, 15000);
    cJSON_free(txt);
    return st;
}

// ---- Speech-to-text ---------------------------------------------------------

static void wav_header(uint8_t h[44], size_t samples)
{
    const uint32_t data = samples * 2, rate = MIC_RATE, byte_rate = MIC_RATE * 2;
    memcpy(h, "RIFF", 4);
    const uint32_t riff = 36 + data;
    memcpy(h + 4, &riff, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    const uint32_t fmt_len = 16;
    const uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
    memcpy(h + 16, &fmt_len, 4);
    memcpy(h + 20, &pcm, 2);
    memcpy(h + 22, &ch, 2);
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &byte_rate, 4);
    memcpy(h + 32, &align, 2);
    memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data, 4);
}

// Tidy a transcript into a task name: trim, collapse spaces, drop a trailing period.
static void tidy(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    bool space = false, cut = false;
    for (const char *p = in; *p; p++) {
        if (isspace((unsigned char)*p)) {
            space = n > 0;
            continue;
        }
        if (n + (space ? 2 : 1) >= cap) {
            cut = true;
            break;
        }
        if (space) out[n++] = ' ';
        space = false;
        out[n++] = *p;
    }
    if (cut) { // don't end in half a UTF-8 character
        size_t k = n;
        while (k > 0 && ((unsigned char)out[k - 1] & 0xC0) == 0x80) k--;
        if (k > 0) {
            const unsigned char lead = (unsigned char)out[k - 1];
            const size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
            if (k - 1 + need > n) n = k - 1;
        }
    }
    while (n > 0 && (out[n - 1] == '.' || out[n - 1] == ' ')) n--;
    out[n] = 0;
}

static stt_result_t transcribe(int16_t *pcm, size_t n, char *out, size_t cap)
{
    if (!mic_prepare(&pcm, &n)) return STT_NO_SPEECH;
    if (!net_connected()) return STT_NO_WIFI;

    static const char B[] = "----knob-pomodoro-7d1f3a";
    char *head = s_head;
    const int cap_head = sizeof s_head;
    for (int with_keywords = s_nproj > 0 && !s_no_keywords; with_keywords >= 0; with_keywords--) {
        int len = snprintf(head, cap_head,
                           "--%s\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n" STT_MODEL "\r\n"
                           "--%s\r\nContent-Disposition: form-data; name=\"response_format\"\r\n\r\njson\r\n",
                           B, B);
        // Toggl project names help the recognizer spell them the way Toggl has them.
        for (int i = 0; with_keywords && i < s_nproj; i++) {
            if (len + 120 + (int)strlen(s_proj[i].name) > cap_head - 200) break; // keep room for the file part
            len += snprintf(head + len, cap_head - len,
                            "--%s\r\nContent-Disposition: form-data; name=\"keywords[]\"\r\n\r\n%s\r\n", B,
                            s_proj[i].name);
        }
        len += snprintf(head + len, cap_head - len,
                        "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"task.wav\"\r\n"
                        "Content-Type: audio/wav\r\n\r\n",
                        B);
        uint8_t wav[44];
        wav_header(wav, n);
        char tail[64];
        const int tlen = snprintf(tail, sizeof tail, "\r\n--%s--\r\n", B);
        char ctype[80];
        snprintf(ctype, sizeof ctype, "multipart/form-data; boundary=%s", B);
        const piece_t pieces[] = {{head, len}, {wav, 44}, {pcm, n * 2}, {tail, tlen}};

        const int64_t t0 = esp_timer_get_time();
        int st = https(HTTP_METHOD_POST, STT_URL, s_openai_auth, ctype, pieces, 4, 30000);
        if (st < 0 && net_connected()) { // dropped mid-upload: one more try
            ESP_LOGW(TAG, "transcription: connection dropped, trying again");
            st = https(HTTP_METHOD_POST, STT_URL, s_openai_auth, ctype, pieces, 4, 30000);
        }
        ESP_LOGI(TAG, "transcription: HTTP %d in %lld ms (%.1f s of audio)", st,
                 (esp_timer_get_time() - t0) / 1000, n / (float)MIC_RATE);
        if (st == 400 && with_keywords) {
            ESP_LOGW(TAG, "rejected with keywords, retrying without: %.200s", s_resp);
            s_no_keywords = true;
            continue;
        }
        if (st != 200) {
            ESP_LOGW(TAG, "transcription failed: %.300s", s_resp);
            return st < 0 && !net_connected() ? STT_NO_WIFI : STT_FAILED;
        }
        cJSON *j = cJSON_Parse(s_resp);
        const cJSON *t = cJSON_GetObjectItem(j, "text");
        if (cJSON_IsString(t)) tidy(t->valuestring, out, cap);
        else out[0] = 0;
        cJSON_Delete(j);
        ESP_LOGI(TAG, "heard: \"%s\"", out);
        return out[0] ? STT_OK : STT_NO_SPEECH;
    }
    return STT_FAILED;
}

// ---- Toggl 2.0 ------------------------------------------------------------------
// API at focus.toggl.com/api with a Bearer key (toggl_sk_...). Almost everything
// lives under /organizations/{org}/workspaces/{ws}/...; the workspace comes from
// the user's settings and the organization from the workspace's context.

static int64_t epoch_at(int64_t at_us)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    const int64_t now_epoch_us = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
    return (now_epoch_us - (esp_timer_get_time() - at_us)) / 1000000;
}

static void iso(int64_t epoch, char out[24])
{
    const time_t t = (time_t)epoch;
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, 24, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

// "2026-10-31T12:34:56(.123)(Z|+02:00)" -> seconds since 1970 (UTC), 0 if unreadable.
static int64_t parse_iso(const char *s)
{
    int y, mo, d, h, mi, sec;
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6) return 0;
    // Days from civil (Howard Hinnant's algorithm).
    y -= mo <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = y - era * 400;
    const int doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t t = ((int64_t)era * 146097 + doe - 719468) * 86400 + h * 3600 + mi * 60 + sec;
    const char *z = s + 19;
    while (*z == '.' || (*z >= '0' && *z <= '9')) z++;
    if (*z == '+' || *z == '-') {
        int oh = 0, om = 0;
        sscanf(z + 1, "%d:%d", &oh, &om);
        t -= (*z == '+' ? 1 : -1) * (oh * 3600 + om * 60);
    }
    return t;
}

static bool http_ok(int st) { return st >= 200 && st < 300; }

// After a failed Toggl call: a rejected key turns Toggl off; network trouble,
// rate limits (429) and server errors back off and retry. Returns true if the
// request is worth retrying.
static bool toggl_failed(int st, const char *what)
{
    if (st == 401) {
        ESP_LOGE(TAG, "Toggl rejected the API key (%s): %.160s", what, s_resp);
        ESP_LOGE(TAG, "time tracking is off until restart");
        s_toggl_off = true;
        return false;
    }
    if (st < 0 || st == 429 || st >= 500) {
        s_next_try = esp_timer_get_time() + (int64_t)s_backoff_s * 1000000;
        if (st == 429) s_rate_until = s_next_try;
        ESP_LOGW(TAG, "Toggl %s: HTTP %d, retrying in %d s: %.160s", what, st, s_backoff_s, s_resp);
        s_backoff_s = s_backoff_s * 2 > 600 ? 600 : s_backoff_s * 2;
        return true;
    }
    ESP_LOGW(TAG, "Toggl %s: HTTP %d: %.240s", what, st, s_resp);
    return false;
}

// A number field, also looked up inside a "data" wrapper.
static int64_t jnum(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    if (!v) {
        const cJSON *d = cJSON_GetObjectItem(o, "data");
        if (cJSON_IsObject(d)) v = cJSON_GetObjectItem(d, key);
    }
    return cJSON_IsNumber(v) ? (int64_t)v->valuedouble : 0;
}

static const char *jstr(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

// strlcpy that never cuts a UTF-8 character in half (the text goes back to Toggl).
static void ucopy(char *dst, const char *src, size_t cap)
{
    if (strlcpy(dst, src, cap) < cap) return;
    size_t n = cap - 1; // src[n] is the first byte left out
    if (((unsigned char)src[n] & 0xC0) != 0x80) return; // a clean cut between characters
    while (n > 0 && ((unsigned char)dst[n - 1] & 0xC0) == 0x80) n--;
    if (n > 0) n--; // drop the character's lead byte too
    dst[n] = 0;
}

// Toggl is rate-limiting us: don't spend another request on a swipe or a list.
static bool rate_limited(const char *what)
{
    const int64_t left = s_rate_until - esp_timer_get_time();
    if (left <= 0) return false;
    ESP_LOGW(TAG, "Toggl %s: rate-limited for another %lld s, not asking", what, left / 1000000);
    return true;
}

static uint32_t parse_color(const char *s)
{
    if (!s) return 0x8E8E94;
    if (*s == '#') s++;
    if (strlen(s) < 6) return 0x8E8E94;
    char hex[7];
    memcpy(hex, s, 6);
    hex[6] = 0;
    return (uint32_t)strtoul(hex, NULL, 16) & 0xFFFFFF;
}

static int cmp_project(const void *a, const void *b)
{
    return strcasecmp(((const project_t *)a)->name, ((const project_t *)b)->name);
}

static int toggl_get(const char *url, const char *what)
{
    const int st = https_json(HTTP_METHOD_GET, url, NULL);
    if (!http_ok(st)) toggl_failed(st, what);
    return st;
}

// Active projects in the workspace, sorted by name, with their Toggl colors.
static void toggl_fetch_projects(void)
{
    char url[200];
    snprintf(url, sizeof url, TOGGL_API "/organizations/%lld/workspaces/%lld/projects?per_page=100", s_oid, s_wid);
    if (!http_ok(toggl_get(url, "projects"))) return;
    project_t *list = s_proj_tmp;
    int n = 0;
    cJSON *root = cJSON_Parse(s_resp);
    const cJSON *arr = cJSON_IsArray(root) ? root : cJSON_GetObjectItem(root, "data"), *p;
    cJSON_ArrayForEach(p, arr)
    {
        const char *name = jstr(p, "name");
        const int64_t id = jnum(p, "id");
        if (n >= MAX_PROJECTS || !id || !name) continue;
        // ("active": false is common for projects people still log to, such as default ones; not a filter.)
        const char *skip = jstr(p, "archived_at")                            ? "archived"
                           : jstr(p, "deleted_at")                           ? "deleted"
                           : cJSON_IsTrue(cJSON_GetObjectItem(p, "is_template")) ? "template"
                           : cJSON_IsTrue(cJSON_GetObjectItem(p, "draft"))       ? "draft"
                                                                                 : NULL;
        if (skip) {
            ESP_LOGI(TAG, "Toggl project \"%s\" left out (%s)", name, skip);
            continue;
        }
        list[n].id = id;
        list[n].color = parse_color(jstr(p, "color"));
        ucopy(list[n].name, name, PROJECT_NAME_LEN);
        n++;
    }
    ESP_LOGI(TAG, "Toggl projects response: %d in this page", cJSON_GetArraySize(arr));
    cJSON_Delete(root);
    qsort(list, n, sizeof list[0], cmp_project);
    xSemaphoreTake(s_proj_lock, portMAX_DELAY);
    memcpy(s_proj, list, n * sizeof list[0]);
    s_nproj = n;
    s_proj_version++;
    s_proj_loaded_us = esp_timer_get_time();
    xSemaphoreGive(s_proj_lock);
    ESP_LOGI(TAG, "Toggl: %d projects", n);
}

// Look for an organization id anywhere in a JSON response (fields Toggl's spec
// doesn't list sometimes appear anyway).
static int64_t scan_org(const char *txt)
{
    static const char *const KEYS[] = {"\"organization_id\"", "\"current_organization_id\"",
                                       "\"default_organization_id\"", "\"mcp_active_organization_id\""};
    for (size_t k = 0; k < sizeof KEYS / sizeof KEYS[0]; k++) {
        for (const char *p = strstr(txt, KEYS[k]); p; p = strstr(p + 1, KEYS[k])) {
            const char *q = p + strlen(KEYS[k]);
            while (*q == ' ' || *q == '\n' || *q == '\t') q++;
            if (*q++ != ':') continue;
            while (*q == ' ' || *q == '\n' || *q == '\t') q++;
            const long long v = strtoll(q, NULL, 10);
            if (v > 0) return v;
        }
    }
    return 0;
}

// Setup problems are retried slowly (from 15 s up to 10 minutes apart).
static bool setup_failed(void)
{
    if (!s_toggl_off) {
        s_next_try = esp_timer_get_time() + (int64_t)s_backoff_s * 1000000;
        s_backoff_s = s_backoff_s * 2 > 600 ? 600 : s_backoff_s * 2;
    }
    return false;
}

static bool toggl_setup(void)
{
    if (!http_ok(toggl_get(TOGGL_API "/users/me/settings", "settings"))) return setup_failed();
    cJSON *j = cJSON_Parse(s_resp);
    const int64_t wid = jnum(j, "current_workspace_id");
    cJSON_Delete(j);
    const int64_t settings_oid = scan_org(s_resp);
    if (!wid) {
        ESP_LOGE(TAG, "Toggl: no current workspace in your settings");
        return setup_failed();
    }
    // The organization: from secrets.h if given, else the one Toggl's AI connector
    // last used, else the workspace's context (which may need a browser session).
    int64_t oid = TOGGL_ORGANIZATION_ID;
    const char *from = "secrets.h";
    if (!oid && settings_oid) {
        oid = settings_oid;
        from = "your Toggl settings";
    }
    // Workspace endpoints that accept an API key; any of them may mention the organization.
    static const char *const PROBES[] = {"/workspaces/%lld/context", "/workspaces/%lld/users/me/settings",
                                         "/workspaces/%lld/settings", "/workspaces/%lld/permissions"};
    for (size_t i = 0; !oid && i < sizeof PROBES / sizeof PROBES[0]; i++) {
        char path[80], url[160];
        snprintf(path, sizeof path, PROBES[i], wid);
        snprintf(url, sizeof url, TOGGL_API "%s", path);
        const int st = https_json(HTTP_METHOD_GET, url, NULL);
        ESP_LOGI(TAG, "Toggl %s: HTTP %d", path, st);
        if (http_ok(st)) oid = scan_org(s_resp);
        if (oid) from = PROBES[i];
    }
    if (!oid) {
        ESP_LOGE(TAG, "Toggl: can't find your organization with an API key. Add its id to secrets.h as "
                      "TOGGL_ORGANIZATION_ID (the first number in focus.toggl.com/<organization>/workspaces/...)");
        return setup_failed();
    }
    ESP_LOGI(TAG, "Toggl organization from %s", from);
    s_wid = wid;
    s_oid = oid;
    ESP_LOGI(TAG, "Toggl ready: organization %lld, workspace %lld", s_oid, s_wid);
    toggl_fetch_projects();
    s_backoff_s = 5;
    return true;
}

static bool toggl_ready(void)
{
    if (!HAVE_TOGGL || s_toggl_off || !net_connected() || !net_time_ok()) return false;
    return s_wid || (esp_timer_get_time() >= s_next_try && toggl_setup());
}

// The longest project name found in the text as whole words (case-insensitive).
int64_t cloud_match_project(const char *desc)
{
    if (!s_proj_lock) return 0;
    xSemaphoreTake(s_proj_lock, portMAX_DELAY);
    int64_t best = 0;
    size_t best_len = 0;
    const size_t dl = strlen(desc);
    for (int i = 0; i < s_nproj; i++) {
        const char *name = s_proj[i].name;
        const size_t nl = strlen(name);
        if (!nl || nl > dl || nl <= best_len) continue;
        for (size_t at = 0; at + nl <= dl; at++) {
            if (strncasecmp(desc + at, name, nl) != 0) continue;
            const bool left = at == 0 || !isalnum((unsigned char)desc[at - 1]);
            const bool right = at + nl == dl || !isalnum((unsigned char)desc[at + nl]);
            if (left && right) {
                best = s_proj[i].id;
                best_len = nl;
                break;
            }
        }
    }
    xSemaphoreGive(s_proj_lock);
    return best;
}

// Recent finished entries, newest first, one row per (label, project).
static void toggl_fetch_entries(void)
{
    if (!toggl_ready() || rate_limited("entries")) {
        __atomic_store_n(&s_entries_state, ENTRIES_FAILED, __ATOMIC_RELEASE);
        return;
    }
    char from[24], to[24], url[320];
    const int64_t now_epoch = epoch_at(esp_timer_get_time());
    iso(now_epoch - 30 * 86400, from);
    iso(now_epoch + 60, to);
    snprintf(url, sizeof url,
             TOGGL_API "/organizations/%lld/workspaces/%lld/time-entries"
                       "?per_page=%d&order_by=-start&include_taskless=true&type=activity&date_from=%s&date_to=%s",
             s_oid, s_wid, ENTRIES_FETCH, from, to);
    const int st = https_json(HTTP_METHOD_GET, url, NULL);
    if (!http_ok(st)) {
        if (st == 401) toggl_failed(st, "entries");
        else ESP_LOGW(TAG, "Toggl entries: HTTP %d: %.200s", st, s_resp);
        __atomic_store_n(&s_entries_state, ENTRIES_FAILED, __ATOMIC_RELEASE);
        return;
    }
    int n = 0;
    cJSON *root = cJSON_Parse(s_resp);
    const cJSON *arr = cJSON_IsArray(root) ? root : cJSON_GetObjectItem(root, "data"), *e;
    cJSON_ArrayForEach(e, arr)
    {
        if (n >= MAX_ENTRIES) break;
        if (jnum(e, "duration") <= 0 || !jstr(e, "start")) continue; // running, or only planned
        const cJSON *task = cJSON_GetObjectItem(e, "task"), *proj = cJSON_GetObjectItem(e, "project");
        const char *desc = jstr(e, "description"), *task_name = cJSON_IsObject(task) ? jstr(task, "name") : NULL;
        const char *proj_name = cJSON_IsObject(proj) ? jstr(proj, "name") : NULL;
        const char *label = desc && desc[0] ? desc : task_name;
        if (!label || !label[0]) continue;
        toggl_entry_t row = {.task_id = jnum(e, "task_id")};
        ucopy(row.label, label, sizeof row.label);
        row.project.id = jnum(e, "project_id");
        if (row.project.id) {
            ucopy(row.project.name, proj_name ? proj_name : "", PROJECT_NAME_LEN);
            row.project.color = parse_color(cJSON_IsObject(proj) ? jstr(proj, "color") : NULL);
        }
        bool dup = false;
        for (int i = 0; i < n && !dup; i++) {
            dup = s_entries_tmp[i].project.id == row.project.id && strcasecmp(s_entries_tmp[i].label, row.label) == 0;
        }
        if (!dup) s_entries_tmp[n++] = row;
    }
    cJSON_Delete(root);
    xSemaphoreTake(s_proj_lock, portMAX_DELAY);
    memcpy(s_entries, s_entries_tmp, n * sizeof(toggl_entry_t));
    s_nentries = n;
    xSemaphoreGive(s_proj_lock);
    __atomic_store_n(&s_entries_state, ENTRIES_OK, __ATOMIC_RELEASE);
    ESP_LOGI(TAG, "Toggl: %d recent entries%s%s", n, n ? ", latest: " : "", n ? s_entries[0].label : "");
}

static cJSON *entry_body(const seg_t *g)
{
    cJSON *b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "type", "activity");
    cJSON_AddStringToObject(b, "description", g->desc);
    if (g->project_id) cJSON_AddNumberToObject(b, "project_id", (double)g->project_id);
    return b;
}

static void ws_url(char *url, size_t cap, const char *path)
{
    snprintf(url, cap, TOGGL_API "/organizations/%lld/workspaces/%lld/%s", s_oid, s_wid, path);
}

static void entry_url(char *url, size_t cap, int64_t id)
{
    snprintf(url, cap, TOGGL_API "/organizations/%lld/workspaces/%lld/time-entries/%lld", s_oid, s_wid, id);
}

// The id of the timer running in Toggl (0 = none), or -1 if Toggl couldn't be asked.
// With `out`, also its details.
static int64_t current_timer(toggl_running_t *out)
{
    char url[200];
    ws_url(url, sizeof url, "tracking/current");
    const int st = https_json(HTTP_METHOD_GET, url, NULL);
    if (!http_ok(st)) {
        toggl_failed(st, "check timer");
        return -1;
    }
    if (st == 204 || !s_resp[0]) return 0;
    cJSON *r = cJSON_Parse(s_resp);
    const cJSON *e = cJSON_IsObject(cJSON_GetObjectItem(r, "data")) ? cJSON_GetObjectItem(r, "data") : r;
    const int64_t id = cJSON_IsObject(e) ? jnum(e, "id") : 0;
    if (id && out) {
        const char *desc = jstr(e, "description"), *start = jstr(e, "start");
        ucopy(out->desc, desc ? desc : "", sizeof out->desc);
        out->project_id = jnum(e, "project_id");
        out->task_id = jnum(e, "task_id");
        const int64_t now = esp_timer_get_time();
        const int64_t start_epoch = start ? parse_iso(start) : 0;
        out->start_us = start_epoch > 0 ? now - (epoch_at(now) - start_epoch) * 1000000 : now;
    }
    cJSON_Delete(r);
    return id;
}

// Stop whatever runs in Toggl, as of `at_us`.
static int stop_running(int64_t at_us)
{
    char url[200], ts[24];
    cJSON *b = cJSON_CreateObject();
    iso(epoch_at(at_us), ts);
    cJSON_AddStringToObject(b, "end", ts);
    ws_url(url, sizeof url, "tracking/stop");
    const int st = https_json(HTTP_METHOD_POST, url, b);
    cJSON_Delete(b);
    return st;
}

// Start a running entry. If Toggl refuses because another timer runs (it runs one at a
// time), stop that one and try again. If nothing runs, the refusal is about the entry
// itself (say, an archived project): don't touch anything else.
static int start_running(cJSON *body)
{
    char url[200];
    ws_url(url, sizeof url, "tracking/start");
    int st = https_json(HTTP_METHOD_POST, url, body);
    if (st == 400 || st == 409 || st == 422) {
        ESP_LOGW(TAG, "Toggl start: HTTP %d (%.160s)", st, s_resp);
        const int refused = st;
        const int64_t running = current_timer(NULL);
        if (running < 0) return -1; // couldn't ask: try the whole start again later
        if (!running) return refused;
        ESP_LOGW(TAG, "Toggl start: stopping the running timer %lld and trying again", running);
        if (http_ok(stop_running(esp_timer_get_time()))) st = https_json(HTTP_METHOD_POST, url, body);
    }
    return st;
}

// A stretch or a change Toggl turned down for good: tell the UI ("not saved in Toggl").
static int dropped(void)
{
    __atomic_add_fetch(&s_dropped, 1, __ATOMIC_RELEASE);
    return 1;
}

// Advance one stretch. Returns 1 when it is done (synced, or dropped after a
// permanent error), 0 when it is waiting, -1 after a temporary failure.
static int sync_seg(seg_t *g, int64_t now)
{
    char url[220], ts[24];
    const bool short_run = !g->adopted && g->stop_us && g->stop_us - g->start_us < MIN_SEG_US;
    if (!g->id) {
        if (short_run) return 1; // over within 10 s and never reached Toggl: nothing to do
        cJSON *b = entry_body(g);
        iso(epoch_at(g->start_us), ts);
        cJSON_AddStringToObject(b, "start", ts);
        int st;
        if (g->stop_us) { // it already ended (we were offline): log it complete
            cJSON_AddNumberToObject(b, "duration", (double)((g->stop_us - g->start_us) / 1000000));
            if (g->task_id) {
                snprintf(url, sizeof url, TOGGL_API "/organizations/%lld/workspaces/%lld/tasks/%lld/time-entries", s_oid,
                         s_wid, g->task_id);
            } else {
                ws_url(url, sizeof url, "time-entries");
            }
            st = https_json(HTTP_METHOD_POST, url, b);
        } else {
            if (g->task_id) cJSON_AddNumberToObject(b, "task_id", (double)g->task_id);
            st = start_running(b);
        }
        cJSON_Delete(b);
        if (!http_ok(st)) return toggl_failed(st, g->stop_us ? "log entry" : "start") ? -1 : dropped();
        cJSON *r = cJSON_Parse(s_resp);
        g->id = jnum(r, "id");
        cJSON_Delete(r);
        g->dirty_desc = g->dirty_start = false;
        ESP_LOGI(TAG, "Toggl entry %lld %s: \"%s\"", g->id, g->stop_us ? "logged" : "started", g->desc);
        if (g->stop_us || !g->id) return 1;
    }
    if (short_run) { // an accidental start: leave nothing behind
        entry_url(url, sizeof url, g->id);
        int st = https_json(HTTP_METHOD_DELETE, url, NULL);
        if (!http_ok(st) && st != 404 && st > 0 && st < 500 && st != 429) {
            // Toggl may not delete a running entry: stop it first (if it's still ours), then delete.
            const int64_t running = current_timer(NULL);
            if (running < 0) return -1;
            if (running == g->id && !http_ok(stop_running(g->stop_us))) {
                return toggl_failed(s_last_status, "stop") ? -1 : dropped();
            }
            st = https_json(HTTP_METHOD_DELETE, url, NULL);
        }
        if (!http_ok(st) && st != 404) return toggl_failed(st, "delete") ? -1 : dropped();
        ESP_LOGI(TAG, "Toggl entry %lld deleted (stopped within 10 s)", g->id);
        return 1;
    }
    if (g->dirty_desc || g->dirty_start) {
        if (!g->stop_us && now - g->changed_us < SETTLE_US) return 0; // more changes may follow (knob clicks)
        // Only what changed: a knob turn mustn't rewrite a description (the knob keeps 95 bytes of it).
        cJSON *b = cJSON_CreateObject();
        if (g->dirty_desc) {
            cJSON_AddStringToObject(b, "description", g->desc);
            if (g->project_id) cJSON_AddNumberToObject(b, "project_id", (double)g->project_id);
            else cJSON_AddNullToObject(b, "project_id");
            if (g->task_id) cJSON_AddNumberToObject(b, "task_id", (double)g->task_id);
        }
        iso(epoch_at(g->start_us), ts);
        if (g->dirty_start) cJSON_AddStringToObject(b, "start", ts);
        entry_url(url, sizeof url, g->id);
        const int st = https_json(HTTP_METHOD_PATCH, url, b);
        cJSON_Delete(b);
        if (!http_ok(st) && toggl_failed(st, "update")) return -1;
        if (!http_ok(st)) dropped();
        else if (g->dirty_desc && g->dirty_start) ESP_LOGI(TAG, "Toggl entry %lld updated (description, project, start): \"%s\", start %s", g->id, g->desc, ts);
        else if (g->dirty_desc) ESP_LOGI(TAG, "Toggl entry %lld updated (description, project): \"%s\"", g->id, g->desc);
        else ESP_LOGI(TAG, "Toggl entry %lld updated (start): start %s", g->id, ts);
        g->dirty_desc = g->dirty_start = false;
    }
    if (g->stop_us) {
        // Toggl 2.0 stops whatever is running, so first make sure that's still ours
        // (it may have been stopped, or another timer started, on another device).
        const int64_t running = current_timer(NULL);
        if (running < 0) return -1;
        if (running != g->id) {
            ESP_LOGI(TAG, "Toggl entry %lld was already stopped elsewhere", g->id);
            return 1;
        }
        const int st = stop_running(g->stop_us);
        if (!http_ok(st)) return toggl_failed(st, "stop") ? -1 : dropped();
        ESP_LOGI(TAG, "Toggl entry %lld stopped", g->id);
        return 1;
    }
    return 0;
}

static void toggl_sync(void)
{
    if (esp_timer_get_time() < s_next_try || !toggl_ready()) return;
    const int64_t now = esp_timer_get_time();
    for (int i = 0; i < s_nseg;) {
        const int r = sync_seg(&s_seg[i], now);
        if (r < 0) return;
        if (r == 0) {
            i++;
            continue;
        }
        memmove(&s_seg[i], &s_seg[i + 1], (s_nseg - i - 1) * sizeof(seg_t));
        s_nseg--;
    }
    s_backoff_s = 5;
}

static seg_t *running_seg(void) { return s_nseg > 0 && !s_seg[s_nseg - 1].stop_us ? &s_seg[s_nseg - 1] : NULL; }

// Swipe down: compare Toggl's running timer with the knob's.
static void toggl_check(bool knob_running)
{
    toggl_sync(); // first send what's queued, so Toggl knows about the knob's timer
    toggl_running_t cur = {0};
    const int64_t id = toggl_ready() && !rate_limited("check") ? current_timer(&cur) : -1;
    if (id < 0) {
        __atomic_store_n(&s_check, SYNC_FAILED, __ATOMIC_RELEASE);
        return;
    }
    seg_t *ours = running_seg();
    sync_result_t r = SYNC_IN_SYNC;
    if (id && (!ours || ours->id != id)) {
        // Another timer runs (Toggl runs one at a time, so the knob's own entry, if any, was stopped): adopt it.
        if (ours) s_nseg--;
        if (s_nseg == MAX_SEGS) {
            memmove(&s_seg[0], &s_seg[1], (MAX_SEGS - 1) * sizeof(seg_t));
            s_nseg--;
        }
        seg_t *g = &s_seg[s_nseg++];
        *g = (seg_t){.start_us = cur.start_us, .id = id, .project_id = cur.project_id, .task_id = cur.task_id,
                     .adopted = true};
        strlcpy(g->desc, cur.desc, sizeof g->desc);
        cur.id = id;
        r = SYNC_ADOPTED;
        ESP_LOGI(TAG, "Toggl: adopted the running entry %lld: \"%s\"", id, cur.desc);
    } else if (!id && ours && ours->id) {
        ESP_LOGI(TAG, "Toggl entry %lld was stopped elsewhere", ours->id);
        s_nseg--;
        r = SYNC_STOPPED;
    } else if (!id && !ours && knob_running) {
        // The knob runs but has no entry (Toggl turned the start down, or it was lost): Toggl wins.
        ESP_LOGI(TAG, "Toggl has no running entry for the knob's timer");
        r = SYNC_STOPPED;
    }
    s_check_info = cur;
    __atomic_store_n(&s_check, r, __ATOMIC_RELEASE);
}

static void handle(const msg_t *m)
{
    switch (m->kind) {
    case MSG_TRANSCRIBE: {
        char text[SCENE_TASK_LEN];
        const stt_result_t r = transcribe(m->pcm, m->n, text, sizeof text);
        if (r == STT_OK) strlcpy(s_stt_text, text, sizeof s_stt_text);
        __atomic_store_n(&s_stt, r, __ATOMIC_RELEASE);
        break;
    }
    case MSG_START:
        if (!HAVE_TOGGL || s_toggl_off) break;
        if (s_nseg > 0 && !s_seg[s_nseg - 1].stop_us) s_seg[s_nseg - 1].stop_us = m->at_us;
        if (s_nseg == MAX_SEGS) { // offline for a long time: drop the oldest
            memmove(&s_seg[0], &s_seg[1], (MAX_SEGS - 1) * sizeof(seg_t));
            s_nseg--;
        }
        s_seg[s_nseg] = (seg_t){.start_us = m->at_us, .project_id = m->project_id, .task_id = m->task_id};
        strlcpy(s_seg[s_nseg].desc, m->text, SCENE_TASK_LEN);
        s_nseg++;
        break;
    case MSG_STOP:
        if (s_nseg > 0 && !s_seg[s_nseg - 1].stop_us) s_seg[s_nseg - 1].stop_us = m->at_us;
        break;
    case MSG_UPDATE:
    case MSG_SHIFT: {
        seg_t *g = running_seg();
        if (!g) break;
        if (m->kind == MSG_UPDATE) {
            strlcpy(g->desc, m->text, SCENE_TASK_LEN);
            g->project_id = m->project_id;
            g->task_id = m->task_id;
        } else {
            g->start_us = m->at_us;
        }
        if (g->id) { // (before it exists, the start request carries the change)
            if (m->kind == MSG_UPDATE) g->dirty_desc = true;
            else g->dirty_start = true;
            g->changed_us = esp_timer_get_time(); // all changes settle together into one PATCH
        }
        break;
    }
    case MSG_CHECK:
        toggl_check(m->knob_running);
        break;
    case MSG_REFRESH:
        if (esp_timer_get_time() - s_proj_loaded_us > PROJECTS_MAX_AGE_US && toggl_ready()) toggl_fetch_projects();
        break;
    case MSG_ENTRIES:
        toggl_fetch_entries();
        break;
    case MSG_DEBUG_START: // a timer started on another device (tests)
        if (toggl_ready()) {
            cJSON *b = cJSON_CreateObject();
            char ts[24];
            cJSON_AddStringToObject(b, "type", "activity");
            cJSON_AddStringToObject(b, "description", m->text);
            iso(epoch_at(m->at_us), ts);
            cJSON_AddStringToObject(b, "start", ts);
            const int st = start_running(b);
            cJSON_Delete(b);
            cJSON *r = cJSON_Parse(s_resp);
            ESP_LOGI(TAG, "debug: started Toggl entry %lld elsewhere (HTTP %d)", http_ok(st) ? jnum(r, "id") : 0, st);
            cJSON_Delete(r);
        }
        break;
    case MSG_DEBUG_STOP: // the running timer stopped on another device (tests)
        if (toggl_ready()) ESP_LOGI(TAG, "debug: stopped the Toggl timer elsewhere (HTTP %d)", stop_running(m->at_us));
        break;
    }
}

static void cloud_task(void *arg)
{
    while (true) {
        msg_t m;
        // Wake at least every 2 s while Toggl has work (or still needs setting up).
        const bool busy = HAVE_TOGGL && !s_toggl_off && (s_nseg > 0 || !s_wid);
        if (xQueueReceive(s_q, &m, busy ? pdMS_TO_TICKS(2000) : portMAX_DELAY)) handle(&m);
        toggl_sync();
    }
}

// ---- API for the UI -----------------------------------------------------------

// Keys pasted from a web page can carry invisible characters (zero-width
// spaces, non-breaking spaces) or stray whitespace. Keep only plain ASCII
// key characters (or, for a classic 32-hex-digit token, just the hex digits).
// Logs the codes of what was removed, never the key.
static void clean_key(const char *in, char *out, size_t cap, bool hex_token, const char *what)
{
    size_t n = 0, hex = 0, removed = 0;
    char codes[64] = "";
    for (const char *p = in; *p; p++) hex += isxdigit((unsigned char)*p) != 0;
    const bool only_hex = hex_token && hex == 32;
    for (const char *p = in; *p && n + 1 < cap; p++) {
        const unsigned char ch = (unsigned char)*p;
        const bool keep = only_hex ? isxdigit(ch) != 0 : (ch > 0x20 && ch < 0x7F);
        if (keep) {
            out[n++] = *p;
        } else {
            removed++;
            const size_t len = strlen(codes);
            if (len + 6 < sizeof codes) snprintf(codes + len, sizeof codes - len, " %02X", ch);
        }
    }
    out[n] = 0;
    if (removed) ESP_LOGW(TAG, "%s: ignored %u stray characters (byte codes:%s)", what, (unsigned)removed, codes);
}

// cJSON makes many small allocations (a project list can be thousands of nodes);
// keep them out of internal RAM, which the display and Wi-Fi need.
static void *json_malloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }

void cloud_init(void)
{
    if (!HAVE_OPENAI && !HAVE_TOGGL) {
        ESP_LOGI(TAG, "no API keys in secrets.h; voice and Toggl are off");
        return;
    }
    cJSON_Hooks hooks = {.malloc_fn = json_malloc, .free_fn = free};
    cJSON_InitHooks(&hooks);
    s_resp = heap_caps_malloc(RESP_CAP, MALLOC_CAP_SPIRAM);
    s_q = xQueueCreateWithCaps(QUEUE_LEN, sizeof(msg_t), MALLOC_CAP_SPIRAM);
    s_proj_lock = xSemaphoreCreateMutex();
    if (HAVE_OPENAI) {
        char key[200];
        clean_key(OPENAI_API_KEY, key, sizeof key, false, "OpenAI key");
        snprintf(s_openai_auth, sizeof s_openai_auth, "Bearer %s", key);
    }
    if (HAVE_TOGGL) {
        char key[80];
        clean_key(TOGGL_API_TOKEN, key, sizeof key, false, "Toggl key");
        snprintf(s_toggl_auth, sizeof s_toggl_auth, "Bearer %s", key);
        s_proj_tmp = heap_caps_calloc(MAX_PROJECTS, sizeof(project_t), MALLOC_CAP_SPIRAM);
        s_entries = heap_caps_calloc(MAX_ENTRIES, sizeof(toggl_entry_t), MALLOC_CAP_SPIRAM);
        s_entries_tmp = heap_caps_calloc(MAX_ENTRIES, sizeof(toggl_entry_t), MALLOC_CAP_SPIRAM);
    }
    // Internal-RAM stack: TLS math with a PSRAM stack made handshakes ~10x slower.
    xTaskCreatePinnedToCore(cloud_task, "cloud", 10240, NULL, 4, NULL, 1);
    ESP_LOGI(TAG, "voice %s, Toggl %s", HAVE_OPENAI ? "on" : "off", HAVE_TOGGL ? "on" : "off");
}

bool cloud_voice_configured(void) { return HAVE_OPENAI; }
bool cloud_online(void) { return net_connected(); }

static bool post(const msg_t *m)
{
    if (!s_q) return false;
    if (xQueueSend(s_q, m, 0) == pdTRUE) return true;
    ESP_LOGW(TAG, "queue full, dropped a request");
    return false;
}

// Knob clicks on a running entry are held here (UI task only) and posted as one
// MSG_SHIFT once the knob rests, or before any other message, so the order holds.
static bool s_shift_held;
static int64_t s_shift_to, s_shift_since;

static void shift_flush(void)
{
    if (!s_shift_held) return;
    s_shift_held = false;
    post(&(msg_t){.kind = MSG_SHIFT, .at_us = s_shift_to});
}

void cloud_tick(void)
{
    if (s_shift_held && esp_timer_get_time() - s_shift_since > SHIFT_REST_US) shift_flush();
}

bool cloud_transcribe(int16_t *pcm, size_t n)
{
    if (!HAVE_OPENAI || !s_q) return false;
    __atomic_store_n(&s_stt, STT_PENDING, __ATOMIC_RELAXED);
    shift_flush();
    return post(&(msg_t){.kind = MSG_TRANSCRIBE, .pcm = pcm, .n = n});
}

stt_result_t cloud_take_transcript(char *out, size_t cap)
{
    const stt_result_t r = __atomic_load_n(&s_stt, __ATOMIC_ACQUIRE);
    if (r == STT_OK) strlcpy(out, s_stt_text, cap);
    if (r != STT_PENDING) __atomic_store_n(&s_stt, STT_PENDING, __ATOMIC_RELAXED);
    return r;
}

void cloud_session_start(const char *description, int64_t project_id, int64_t task_id, int64_t at_us)
{
    if (!HAVE_TOGGL) return;
    msg_t m = {.kind = MSG_START, .at_us = at_us, .project_id = project_id, .task_id = task_id};
    strlcpy(m.text, description, sizeof m.text);
    shift_flush();
    post(&m);
}

void cloud_session_stop(int64_t at_us)
{
    if (!HAVE_TOGGL) return;
    shift_flush();
    post(&(msg_t){.kind = MSG_STOP, .at_us = at_us});
}

void cloud_session_update(const char *description, int64_t project_id, int64_t task_id)
{
    if (!HAVE_TOGGL) return;
    msg_t m = {.kind = MSG_UPDATE, .project_id = project_id, .task_id = task_id};
    strlcpy(m.text, description, sizeof m.text);
    shift_flush();
    post(&m);
}

void cloud_session_shift(int64_t start_us)
{
    if (!HAVE_TOGGL) return;
    s_shift_held = true;
    s_shift_to = start_us;
    s_shift_since = esp_timer_get_time();
}

bool cloud_check_running(bool knob_running)
{
    if (!HAVE_TOGGL || !s_q) return false;
    shift_flush();
    __atomic_store_n(&s_check, SYNC_PENDING, __ATOMIC_RELEASE);
    if (post(&(msg_t){.kind = MSG_CHECK, .knob_running = knob_running})) return true;
    __atomic_store_n(&s_check, SYNC_IDLE, __ATOMIC_RELEASE);
    return false;
}

bool cloud_take_dropped(void)
{
    static int seen;
    const int n = __atomic_load_n(&s_dropped, __ATOMIC_ACQUIRE);
    if (n == seen) return false;
    seen = n;
    return true;
}

sync_result_t cloud_take_check(toggl_running_t *out)
{
    const sync_result_t r = __atomic_load_n(&s_check, __ATOMIC_ACQUIRE);
    if (r == SYNC_IDLE || r == SYNC_PENDING) return r;
    if (out) *out = s_check_info;
    __atomic_store_n(&s_check, SYNC_IDLE, __ATOMIC_RELAXED);
    return r;
}

void cloud_debug_start_elsewhere(const char *description)
{
    if (!HAVE_TOGGL) return;
    msg_t m = {.kind = MSG_DEBUG_START, .at_us = esp_timer_get_time()};
    strlcpy(m.text, description, sizeof m.text);
    post(&m);
}

void cloud_debug_stop_elsewhere(void)
{
    if (HAVE_TOGGL) post(&(msg_t){.kind = MSG_DEBUG_STOP, .at_us = esp_timer_get_time()});
}

int64_t cloud_running_entry(void)
{
    const int n = __atomic_load_n(&s_nseg, __ATOMIC_ACQUIRE);
    return n > 0 && !s_seg[n - 1].stop_us ? s_seg[n - 1].id : 0;
}

bool cloud_toggl_configured(void) { return HAVE_TOGGL; }

int cloud_projects(project_t *out, int max, uint32_t *version)
{
    if (!s_proj_lock) {
        *version = 0;
        return 0;
    }
    xSemaphoreTake(s_proj_lock, portMAX_DELAY);
    const int n = s_nproj < max ? s_nproj : max;
    memcpy(out, s_proj, n * sizeof(project_t));
    *version = s_proj_version;
    xSemaphoreGive(s_proj_lock);
    return n;
}

void cloud_refresh_projects(void)
{
    if (HAVE_TOGGL) post(&(msg_t){.kind = MSG_REFRESH});
}

uint32_t cloud_projects_version(void) { return __atomic_load_n(&s_proj_version, __ATOMIC_ACQUIRE); }

bool cloud_fetch_entries(void)
{
    if (!HAVE_TOGGL || !s_q) return false;
    __atomic_store_n(&s_entries_state, ENTRIES_LOADING, __ATOMIC_RELEASE);
    if (post(&(msg_t){.kind = MSG_ENTRIES})) return true;
    __atomic_store_n(&s_entries_state, ENTRIES_FAILED, __ATOMIC_RELEASE);
    return false;
}

entries_state_t cloud_entries(toggl_entry_t *out, int max, int *n)
{
    const entries_state_t st = __atomic_load_n(&s_entries_state, __ATOMIC_ACQUIRE);
    *n = 0;
    if (st == ENTRIES_OK && s_proj_lock) {
        xSemaphoreTake(s_proj_lock, portMAX_DELAY);
        *n = s_nentries < max ? s_nentries : max;
        memcpy(out, s_entries, *n * sizeof(toggl_entry_t));
        xSemaphoreGive(s_proj_lock);
    }
    return st;
}

bool cloud_toggl_ready(void) { return HAVE_TOGGL && !s_toggl_off && s_wid != 0; }

void cloud_debug_fields(void *json_object)
{
    cJSON *j = json_object;
    cJSON_AddBoolToObject(j, "toggl_ready", cloud_toggl_ready());
    cJSON_AddBoolToObject(j, "toggl_off", s_toggl_off);
    cJSON_AddNumberToObject(j, "toggl_pending", s_nseg);
    cJSON_AddNumberToObject(j, "toggl_backoff_s", s_next_try > esp_timer_get_time() ? s_backoff_s : 0);
    cJSON_AddNumberToObject(j, "http_last_status", s_last_status);
    cJSON_AddNumberToObject(j, "http_last_ms", (double)s_last_http_ms);
    cJSON_AddNumberToObject(j, "http_errors", s_http_errors);
}
