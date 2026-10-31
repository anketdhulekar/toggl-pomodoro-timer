// DRV2605 haptic driver with an ERM motor (setup matches Waveshare's demo).
#include "haptics.h"

#include <stdbool.h>

#include "board.h"
#include "esp_log.h"

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

// Effect numbers from the DRV2605 ERM waveform library; 0 ends a sequence.
static const uint8_t SEQS[HAP_COUNT][8] = {
    [HAP_CLICK] = {24},                                                // Sharp Tick 1
    [HAP_BUMP] = {7},                                                  // Soft Bump
    [HAP_TAP] = {1},                                                   // Strong Click
    [HAP_DOUBLE] = {10},                                               // Double Click
    [HAP_ALARM] = {15, WAIT_MS(250), 15, WAIT_MS(250), 15},            // 750 ms Alert x3
};

static i2c_master_dev_handle_t s_dev;
static bool s_ok;

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

void haptics_play(haptic_t h)
{
    if (!s_ok || h >= HAP_COUNT) return;
    wr(REG_GO, 0);
    for (int i = 0; i < 8; i++) {
        wr(REG_WAVESEQ1 + i, SEQS[h][i]);
        if (SEQS[h][i] == 0) break;
    }
    wr(REG_GO, 1);
}
