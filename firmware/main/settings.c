#include "settings.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "settings";
static const char *NS = "pomodoro";

void settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void settings_load(settings_t *s)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return; // first boot: keep defaults
    uint8_t v;
    if (nvs_get_u8(h, "set_min", &v) == ESP_OK) s->set_min = v;
    if (nvs_get_u8(h, "app", &v) == ESP_OK) s->app = v;
    if (nvs_get_u8(h, "theme", &v) == ESP_OK) s->theme = v;
    size_t len = sizeof s->task;
    if (nvs_get_str(h, "task", s->task, &len) != ESP_OK) s->task[0] = 0;
    nvs_get_i64(h, "task_id", &s->task_id);
    if (nvs_get_u8(h, "telemetry", &v) == ESP_OK) s->telemetry = v;
    project_t p = {0};
    len = sizeof p.name;
    if (nvs_get_i64(h, "proj_id", &p.id) == ESP_OK) {
        if (p.id <= 0) {
            s->project = (project_t){0}; // "No project" was chosen
        } else if (nvs_get_u32(h, "proj_color", &p.color) == ESP_OK && nvs_get_str(h, "proj_name", p.name, &len) == ESP_OK) {
            s->project = p;
        }
    }
    nvs_close(h);
}

void settings_save(const settings_t *s)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "set_min", (uint8_t)s->set_min);
    nvs_set_u8(h, "app", (uint8_t)s->app);
    nvs_set_u8(h, "theme", (uint8_t)s->theme);
    nvs_set_str(h, "task", s->task);
    nvs_set_i64(h, "task_id", s->task_id);
    nvs_set_u8(h, "telemetry", s->telemetry);
    if (s->project.id >= 0) nvs_set_i64(h, "proj_id", s->project.id); // (-1: the default, not chosen yet)
    nvs_set_u32(h, "proj_color", s->project.color);
    nvs_set_str(h, "proj_name", s->project.name);
    if (nvs_commit(h) == ESP_OK) {
        ESP_LOGI(TAG, "saved: %s, %s, %d min, task \"%s\", project \"%s\"", s->app ? "pomodoro" : "tracking",
                 s->theme ? "inverted" : "mono", s->set_min, s->task, s->project.id > 0 ? s->project.name : "-");
    }
    nvs_close(h);
}
