#pragma once
#include <stdbool.h>
#include "driver/i2c_master.h"

typedef enum {
    HAP_CLICK,  // knob detent
    HAP_BUMP,   // limit reached / action not available
    HAP_TAP,    // start, pause, profile change
    HAP_DOUBLE, // reset
    HAP_COUNT
} haptic_t;

void haptics_init(i2c_master_bus_handle_t bus);
void haptics_play(haptic_t h); // (skipped while a buzz runs)
void haptics_buzz(bool on);    // continuous vibration at full intensity until turned off (time's up)
