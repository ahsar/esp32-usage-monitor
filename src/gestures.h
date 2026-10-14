#pragma once
#include <stdint.h>
#include "util/gesture_util.h"

// Single IMU sampling point. easter_egg.cpp used to poll the accelerometer for
// shake detection itself; that poll now lives here so shake, double-tap,
// pickup, face-down and rotate all read the IMU once instead of competing for
// it. main.cpp calls gesturesUpdate() every loop() and consumes the events.
//
// Events are latched between gesturesUpdate() and gesturesTakeEvents() so a
// burst (e.g. shake + face-up) is never lost.

void gesturesInit();

// Disable everything (e.g. if a config option turns gestures off). Latched
// events keep working; no new samples are taken.
void gesturesSetEnabled(bool enabled);

// Poll the IMU (throttled internally) and latch any gestures.
void gesturesUpdate(uint32_t now);

// True while the device rests face-down (main.cpp blanks the panel).
bool gesturesFaceDown();

// Take and clear the latched gesture::EV_* flags.
uint8_t gesturesTakeEvents();
