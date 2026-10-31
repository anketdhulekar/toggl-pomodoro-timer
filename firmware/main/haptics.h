#pragma once
#include "driver/i2c_master.h"

typedef enum {
    HAP_CLICK,  // knob detent
    HAP_BUMP,   // limit reached / action not available
    HAP_TAP,    // start, pause, profile change
    HAP_DOUBLE, // reset
    HAP_ALARM,  // time's up: three long pulses
    HAP_COUNT
} haptic_t;

void haptics_init(i2c_master_bus_handle_t bus);
void haptics_play(haptic_t h);
