#pragma once
// Coarse periodic-timer helper. Fires immediately on first use (pending), then
// once per period. Wrap-safe via unsigned subtraction.
#include <stdint.h>

struct Interval {
  uint32_t periodMs;
  uint32_t lastMs;
  bool     pending;

  explicit Interval(uint32_t period) : periodMs(period), lastMs(0), pending(true) {}

  bool due(uint32_t now) const {
    return pending || (uint32_t)(now - lastMs) >= periodMs;
  }
  void ran(uint32_t now) {
    lastMs = now;
    pending = false;
  }
  void forcePending() { pending = true; }
};
