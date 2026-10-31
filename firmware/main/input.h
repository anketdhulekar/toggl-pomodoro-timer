#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"

typedef enum {
    TOUCH_NONE,
    TOUCH_TAP,
    TOUCH_HOLD,
    TOUCH_SWIPE_LEFT,
    TOUCH_SWIPE_RIGHT,
    TOUCH_SWIPE_UP,
    TOUCH_SWIPE_DOWN,
} touch_event_t;

typedef struct {
    bool active; // finger is down
    bool moved;  // moved far enough to count as a swipe, not a tap/hold
    bool fired;  // hold already triggered for this press
    int64_t start_us;
} press_state_t;

#define INPUT_HOLD_US 700000

void input_init(i2c_master_bus_handle_t bus);
void input_set_rotation(int deg); // 0 or 180; touch coordinates follow the interface
int input_take_knob_steps(void);                // net detents since last call: + right, - left
touch_event_t input_poll_touch(int64_t now_us); // call every ~15 ms
const press_state_t *input_press(void);
void input_press_origin(int *x, int *y); // where the last press started, in interface pixels
void input_debug_origin(int x, int y);   // debug: pretend the last press started here
