#include "net.h"

#include <string.h>
#include <time.h>

#include "config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"

static const char *TAG = "net";

static volatile bool s_up;
static bool s_sntp_started;
static int s_backoff_ms = 1000;
static esp_timer_handle_t s_retry;
static esp_netif_t *s_netif;

// Public resolvers: the router's DNS took ~7 s to answer the first lookup of each name.
static void set_dns(void)
{
    esp_netif_dns_info_t dns = {.ip.type = ESP_IPADDR_TYPE_V4};
    dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(1, 1, 1, 1);
    esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns);
    dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(8, 8, 8, 8);
    esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_BACKUP, &dns);
}

static volatile bool s_debug_off; // debug: Wi-Fi switched off by a test

static void retry_cb(void *arg)
{
    if (!s_debug_off) esp_wifi_connect();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (!s_debug_off) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        if (s_up || s_backoff_ms == 1000) ESP_LOGW(TAG, "Wi-Fi disconnected (reason %d), retrying", d->reason);
        s_up = false;
        esp_timer_start_once(s_retry, (uint64_t)s_backoff_ms * 1000);
        s_backoff_ms = s_backoff_ms * 2 > 30000 ? 30000 : s_backoff_ms * 2;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "Wi-Fi connected, IP " IPSTR, IP2STR(&e->ip_info.ip));
        set_dns();
        s_up = true;
        s_backoff_ms = 1000;
        if (!s_sntp_started) {
            esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            esp_netif_sntp_init(&cfg);
            s_sntp_started = true;
        }
    }
}

void net_init(void)
{
    if (!HAVE_WIFI) {
        ESP_LOGI(TAG, "no Wi-Fi network in secrets.h; voice and Toggl are off");
        return;
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_netif, "knob-pomodoro");
    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM)); // settings come from secrets.h, not flash
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    const esp_timer_create_args_t targs = {.callback = retry_cb, .name = "wifi_retry"};
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_retry));

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, WIFI_SSID, sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, WIFI_PASSWORD, sizeof wc.sta.password);
    wc.sta.threshold.authmode = sizeof(WIFI_PASSWORD) > 1 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH; // WPA3 networks too
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    // The knob runs on USB power: skip modem sleep, which slows uploads a lot.
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "joining Wi-Fi \"%s\"", WIFI_SSID);
}

bool net_configured(void) { return HAVE_WIFI; }
bool net_connected(void) { return s_up; }

bool net_time_ok(void) { return time(NULL) > 1735689600; } // after 2025-01-01

int net_rssi(void)
{
    wifi_ap_record_t ap;
    return s_up && esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}

void net_debug_enable(bool on)
{
    if (!HAVE_WIFI) return;
    s_debug_off = !on;
    if (on) {
        ESP_LOGI(TAG, "debug: Wi-Fi back on");
        esp_wifi_connect();
    } else {
        ESP_LOGI(TAG, "debug: Wi-Fi off");
        esp_wifi_disconnect();
    }
}
