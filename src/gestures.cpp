#include "gestures.h"
#include "hw/imu.h"
#include <Arduino.h>

static gesture::Engine s_engine;
static uint32_t s_lastPoll = 0;
static uint8_t  s_events   = gesture::EV_NONE;
static bool     s_enabled  = true;

// ~33 Hz: fine enough to resolve a double-tap gap and cheap on the 400 kHz I2C
// bus (12 bytes per read). The QMI8658 is configured for 125 Hz ODR.
static const uint32_t POLL_MS = 30;

void gesturesInit() {
  s_engine.reset();
  s_lastPoll = 0;
  s_events   = gesture::EV_NONE;
}

void gesturesSetEnabled(bool enabled) { s_enabled = enabled; }

bool gesturesFaceDown() { return s_engine.faceDown(); }

uint8_t gesturesTakeEvents() {
  uint8_t e = s_events;
  s_events = gesture::EV_NONE;
  return e;
}

void gesturesUpdate(uint32_t now) {
  if (!s_enabled) return;
  if (s_lastPoll != 0 && (uint32_t)(now - s_lastPoll) < POLL_MS) return;
  s_lastPoll = now;

  gesture::Sample s = {};
  hwImuAccel(&s.ax, &s.ay, &s.az);
  float gx, gy, gz;
  if (hwImuGyro(&gx, &gy, &gz)) {
    s.gx = gx; s.gy = gy; s.gz = gz;
  }
  s.t = now;
  s_events |= s_engine.feed(s);
}
