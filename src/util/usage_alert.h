#pragma once
// Threshold crossings for the usage meters: one cue when a meter first reaches
// 80% and one at 100%, then silence until the window rolls over.
//
// The device is a passive display everywhere else, so this is the one place it
// acts on the numbers it draws. Pure and hardware-free: the host tests drive
// every edge, and main.cpp owns the chime. The meter's `used` counter is what
// detects a reset (a drop), not `resetSec` — two consecutive windows of the same
// length report the same countdown, so the countdown cannot tell them apart.
#include <stdint.h>

namespace usage_alert {

constexpr int WARN_PCT = 80;
constexpr int FULL_PCT = 100;

struct State {
  bool     seen    = false;   // a meter has been observed at least once
  int      lastPct = 0;
  uint64_t lastUsed = 0;
  bool     fired80  = false;
  bool     fired100 = false;
};

enum Event : uint8_t {
  EV_NONE = 0,
  EV_WARN,   // crossed WARN_PCT
  EV_FULL,   // crossed FULL_PCT
};

// Feed one successful usage poll. `usedMicroCents` is the rolling meter's spend.
//
// The first sample after boot only seeds: a device that powers on into a 96%
// meter has not *crossed* anything, and an unconfigured one must not beep at
// every power-on. A drop in spend is the window resetting, which re-arms both
// thresholds.
inline Event tick(State& s, int pct, uint64_t usedMicroCents) {
  if (!s.seen) {
    s.seen    = true;
    s.lastPct = pct;
    s.lastUsed = usedMicroCents;
    s.fired80  = pct >= WARN_PCT;
    s.fired100 = pct >= FULL_PCT;
    return EV_NONE;
  }

  if (usedMicroCents < s.lastUsed) {   // the window reset
    s.fired80  = false;
    s.fired100 = false;
  }

  Event ev = EV_NONE;
  if (!s.fired100 && pct >= FULL_PCT) {
    s.fired100 = true;
    s.fired80  = true;      // a jump straight past 80 is not two cues
    ev = EV_FULL;
  } else if (!s.fired80 && pct >= WARN_PCT) {
    s.fired80 = true;
    ev = EV_WARN;
  }

  s.lastPct  = pct;
  s.lastUsed = usedMicroCents;
  return ev;
}

// Forget everything (a token death, or a device that will be re-authorised):
// the next meter is treated as a fresh seed rather than a crossing.
inline void reset(State& s) { s = State(); }

}  // namespace usage_alert
