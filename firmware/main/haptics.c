// DRV2605 haptic driver with an ERM motor (setup matches Waveshare's demo).
#include "haptics.h"

#include <stdbool.h>

#include "board.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "haptics";

enum {
    REG_STATUS = 0x00,
    REG_MODE = 0x01,
    REG_RTPIN = 0x02,
    REG_LIBRARY = 0x03,
    REG_WAVESEQ1 = 0x04,
    REG_GO = 0x0C,
    REG_OVERDRIVE = 0x0D,
    REG_SUSTAINPOS = 0x0E,
    REG_SUSTAINNEG = 0x0F,
    REG_BREAK = 0x10,
    REG_AUDIOMAX = 0x13,
    REG_FEEDBACK = 0x1A,
    REG_CONTROL3 = 0x1D,
};

#define WAIT_MS(ms) (0x80 | ((ms) / 10))

// Each motor kick draws a pulse of current. On battery, a full-strength tick per knob
// detent dipped the supply enough to make the backlight flicker during a turn. So
// detents use a softer tick, and at most one starts per CLICK_GAP_US.
#define CLICK_GAP_US 40000

// Effect numbers from the DRV2605 ERM waveform library; 0 ends a sequence.
static const uint8_t SEQS[HAP_COUNT][8] = {
    [HAP_CLICK] = {26},                                                // Sharp Tick 3 (60%)
    [HAP_BUMP] = {7},                                                  // Soft Bump
    [HAP_TAP] = {1},                                                   // Strong Click
    [HAP_DOUBLE] = {10},                                               // Double Click
};

static i2c_master_dev_handle_t s_dev;
static bool s_ok;
static bool s_buzzing; // real-time playback at full scale (haptics_buzz)

static esp_err_t wr(uint8_t reg, uint8_t v)
{
    uint8_t b[2] = {reg, v};
    return i2c_master_transmit(s_dev, b, 2, 20);
}

static int rd(uint8_t reg)
{
    uint8_t v;
    return i2c_master_transmit_receive(s_dev, &reg, 1, &v, 1, 20) == ESP_OK ? v : -1;
}

void haptics_init(i2c_master_bus_handle_t bus)
{
    const i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = DRV2605_ADDR,
        .scl_speed_hz = 300000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev, &s_dev));
    const int status = rd(REG_STATUS);
    if (status < 0) {
        ESP_LOGW(TAG, "DRV2605 not found at 0x%02X; vibration disabled", DRV2605_ADDR);
        return;
    }
    wr(REG_MODE, 0x00); // out of standby, internal trigger
    wr(REG_RTPIN, 0x00);
    wr(REG_WAVESEQ1, 1);
    wr(REG_WAVESEQ1 + 1, 0);
    wr(REG_OVERDRIVE, 0);
    wr(REG_SUSTAINPOS, 0);
    wr(REG_SUSTAINNEG, 0);
    wr(REG_BREAK, 0);
    wr(REG_AUDIOMAX, 0x64);
    const int fb = rd(REG_FEEDBACK);
    if (fb >= 0) wr(REG_FEEDBACK, fb & 0x7F); // ERM mode
    const int c3 = rd(REG_CONTROL3);
    if (c3 >= 0) wr(REG_CONTROL3, c3 | 0x20); // ERM open loop
    wr(REG_LIBRARY, 5);
    s_ok = true;
    ESP_LOGI(TAG, "DRV2605 ready (device id %d)", status >> 5);
}

// Continuous vibration at full scale, using real-time playback (RTP): the motor runs at the
// driver's full output (its open-loop clamp, the same level as the strongest library effect)
// until switched off.
void haptics_buzz(bool on)
{
    if (!s_ok || on == s_buzzing) return;
    s_buzzing = on;
    if (on) {
        wr(REG_GO, 0);
        wr(REG_RTPIN, 0x7F); // full scale (signed data format, the default)
        wr(REG_MODE, 0x05);  // real-time playback
    } else {
        wr(REG_RTPIN, 0);
        wr(REG_MODE, 0x00); // back to internal trigger (library effects)
    }
}

void haptics_play(haptic_t h)
{
    // While the time's-up buzz runs, other feedback (a list closing by itself, a sync
    // result) is skipped. Touches and knob turns stop the buzz first, so theirs still plays.
    if (!s_ok || h >= HAP_COUNT || s_buzzing) return;
    if (h == HAP_CLICK) { // a fast spin: skip ticks that would restart the motor right away
        static int64_t last_click_us = -CLICK_GAP_US;
        const int64_t now = esp_timer_get_time();
        if (now - last_click_us < CLICK_GAP_US) return;
        last_click_us = now;
    }
    wr(REG_GO, 0);
    for (int i = 0; i < 8; i++) {
        wr(REG_WAVESEQ1 + i, SEQS[h][i]);
        if (SEQS[h][i] == 0) break;
    }
    wr(REG_GO, 1);
}
