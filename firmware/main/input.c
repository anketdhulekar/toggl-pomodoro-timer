#include "input.h"

#include <stdlib.h>

#include "board.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "input";

#define MOVE_PX 14  // movement that turns a press into a swipe
#define SWIPE_PX 50 // horizontal travel needed for a swipe
#define RELEASE_POLLS 2

// ---- Knob ----------------------------------------------------------------
// Each direction pulses its own line low. A step counts when a line returns
// high after being low for at least two 3 ms samples (debounce), matching
// Waveshare's driver for this knob.

typedef struct {
    uint8_t prev;
    uint8_t low;
} knob_chan_t;

static knob_chan_t s_a, s_b;
static int s_knob_steps;

static void knob_chan(knob_chan_t *c, int level, int dir)
{
    if (level == 0) {
        if (c->prev == 0) {
            if (c->low < 255) c->low++;
        } else {
            c->low = 0;
        }
    } else if (c->prev == 0) {
        if (c->low >= 1) __atomic_add_fetch(&s_knob_steps, dir, __ATOMIC_RELAXED);
        c->low = 0;
    }
    c->prev = (uint8_t)level;
}

static void knob_poll(void *arg)
{
    knob_chan(&s_a, gpio_get_level(PIN_KNOB_A), +1);
    knob_chan(&s_b, gpio_get_level(PIN_KNOB_B), -1);
}

int input_take_knob_steps(void)
{
    return __atomic_exchange_n(&s_knob_steps, 0, __ATOMIC_RELAXED);
}

// ---- Touch ---------------------------------------------------------------

static i2c_master_dev_handle_t s_touch;
static press_state_t s_press;
static bool s_rot180;

void input_set_rotation(int deg) { s_rot180 = (deg % 360) == 180; }
static int s_x0, s_y0, s_lx, s_ly, s_up_polls;

static bool touch_read(int *x, int *y)
{
    uint8_t reg = 0x00, d[7];
    if (i2c_master_transmit_receive(s_touch, &reg, 1, d, sizeof d, 20) != ESP_OK) return false;
    if ((d[2] & 0x0F) == 0) return false;
    *x = ((d[3] & 0x0F) << 8) | d[4];
    *y = ((d[5] & 0x0F) << 8) | d[6];
    if (s_rot180) { // report in the coordinates of the rotated interface
        *x = (LCD_H_RES - 1) - *x;
        *y = (LCD_V_RES - 1) - *y;
    }
    return true;
}

touch_event_t input_poll_touch(int64_t now)
{
    int x, y;
    touch_event_t ev = TOUCH_NONE;
    if (touch_read(&x, &y)) {
        s_up_polls = 0;
        if (!s_press.active) {
            s_press = (press_state_t){.active = true, .start_us = now};
            s_x0 = x;
            s_y0 = y;
        }
        s_lx = x;
        s_ly = y;
        const int dx = x - s_x0, dy = y - s_y0;
        if (!s_press.moved && !s_press.fired) {
            if (dx * dx + dy * dy > MOVE_PX * MOVE_PX) {
                s_press.moved = true;
            } else if (now - s_press.start_us >= INPUT_HOLD_US) {
                s_press.fired = true;
                ev = TOUCH_HOLD;
            }
        }
    } else if (s_press.active && ++s_up_polls >= RELEASE_POLLS) {
        const int dx = s_lx - s_x0, dy = s_ly - s_y0;
        if (s_press.moved) {
            if (abs(dx) > SWIPE_PX && abs(dx) * 10 > abs(dy) * 13) {
                ev = dx < 0 ? TOUCH_SWIPE_LEFT : TOUCH_SWIPE_RIGHT;
            } else if (abs(dy) > SWIPE_PX && abs(dy) * 10 > abs(dx) * 13) {
                ev = dy < 0 ? TOUCH_SWIPE_UP : TOUCH_SWIPE_DOWN;
            }
        } else if (!s_press.fired) {
            ev = TOUCH_TAP;
        }
        s_press.active = false;
    }
    return ev;
}

const press_state_t *input_press(void) { return &s_press; }

void input_debug_origin(int x, int y)
{
    s_x0 = x;
    s_y0 = y;
}

void input_press_origin(int *x, int *y)
{
    *x = s_x0;
    *y = s_y0;
}

void input_init(i2c_master_bus_handle_t bus)
{
    // Knob lines
    const gpio_config_t knob_io = {
        .pin_bit_mask = (1ULL << PIN_KNOB_A) | (1ULL << PIN_KNOB_B),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&knob_io));
    s_a.prev = (uint8_t)gpio_get_level(PIN_KNOB_A);
    s_b.prev = (uint8_t)gpio_get_level(PIN_KNOB_B);
    const esp_timer_create_args_t targs = {.callback = knob_poll, .name = "knob"};
    esp_timer_handle_t t;
    ESP_ERROR_CHECK(esp_timer_create(&targs, &t));
    ESP_ERROR_CHECK(esp_timer_start_periodic(t, 3000));

    // Touch controller: reset pulse, then switch to normal mode
    const gpio_config_t rst_io = {.pin_bit_mask = 1ULL << PIN_TOUCH_RST, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&rst_io);
    gpio_set_level(PIN_TOUCH_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(60));

    const i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TOUCH_ADDR,
        .scl_speed_hz = 300000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev, &s_touch));
    uint8_t normal_mode[2] = {0x00, 0x00};
    if (i2c_master_transmit(s_touch, normal_mode, 2, 50) != ESP_OK) {
        ESP_LOGW(TAG, "touch controller did not answer at 0x%02X (it may be asleep until touched)", TOUCH_ADDR);
    }
    ESP_LOGI(TAG, "knob on GPIO %d/%d, touch on I2C 0x%02X", PIN_KNOB_A, PIN_KNOB_B, TOUCH_ADDR);
}
