#pragma once
// The device channel's agent-state machine, separated from the transport and
// from the UI. It used to be three statics plus an `enterActive()` inside
// status_channel.cpp; nothing about the transitions needs Arduino, so they live
// here and the host tests can drive every edge (duplicate reports, waiting after
// running, a done event with no matching start, …).
//
// `now` is millis() in the firmware and any monotonic tick in the tests. Only
// the *since* timestamp uses it; no transition depends on real time.
#include <stdint.h>
#include <string.h>

enum StatusState : uint8_t {
  STATUS_IDLE = 0,
  STATUS_RUNNING,
  STATUS_WAITING,
};

// One-shot events, consumed by loop() to fire the right chime.
// statusMachineClear() never produces one, so dismissing the animation by hand
// stays silent.
enum StatusEvent : uint8_t {
  STATUS_EV_NONE = 0,
  STATUS_EV_DONE,
  STATUS_EV_FAILED,
  STATUS_EV_WAITING,
};

struct StatusMachine {
  StatusState state = STATUS_IDLE;
  uint32_t    since = 0;                 // millis() of the last state change
  StatusEvent pending = STATUS_EV_NONE;  // not yet consumed by loop()
};

// Apply one *authenticated* state ping. Returns true when the state changed, so
// the caller can wake the panel.
//
//   running       → active; a duplicate running is not a transition
//   waiting       → active, and queues the needs-you chime on the way in
//   done/failed   → idle, queuing a completion event only if it was active
//
// Matches the behaviour the transport had when these were its own statics,
// including the deliberate quiet case: a done/failed with no active session is
// not a chime.
inline bool statusMachineApply(StatusMachine& m, const char* state, uint32_t now) {
  if (!state) return false;

  if (!strcmp(state, "running")) {
    if (m.state == STATUS_RUNNING) return false;
    m.state = STATUS_RUNNING;
    m.since = now;
    return true;
  }

  if (!strcmp(state, "waiting")) {
    if (m.state != STATUS_WAITING) m.pending = STATUS_EV_WAITING;
    if (m.state == STATUS_WAITING) return false;
    m.state = STATUS_WAITING;
    m.since = now;
    return true;
  }

  // done / failed
  if (m.state != STATUS_IDLE) {
    m.pending = strcmp(state, "failed") ? STATUS_EV_DONE : STATUS_EV_FAILED;
  }
  m.state = STATUS_IDLE;
  return false;
}

inline bool statusMachineThinking(const StatusMachine& m) {
  return m.state == STATUS_RUNNING || m.state == STATUS_WAITING;
}

inline bool statusMachineWaiting(const StatusMachine& m) {
  return m.state == STATUS_WAITING;
}

// Key1's "dismiss": drop the animation without producing an event. The pending
// event is left alone, exactly like statusChannelClearThinking().
inline void statusMachineClear(StatusMachine& m) { m.state = STATUS_IDLE; }

inline StatusEvent statusMachineTakeEvent(StatusMachine& m) {
  StatusEvent e = m.pending;
  m.pending = STATUS_EV_NONE;
  return e;
}
