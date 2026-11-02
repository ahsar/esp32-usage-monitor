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

// One-shot events, consumed by loop() to fire the right chime. Hiding the agent
// screen (Key1) never produces one: `statusChannelClearThinking()` only sets the
// dismissed flag, so dismissing a screen stays silent while the machine keeps
// following the sessions.
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
//
// `allowCue` false applies the same transition without queueing anything: it is
// what the session-expiry path needs, because a session that went quiet is not a
// completion and must not ring — while re-deriving the aggregate for the
// *remaining* sessions has to keep its original `since`, or every zombie would
// replay the "it started working" hold.
inline bool statusMachineApply(StatusMachine& m, const char* state, uint32_t now,
                               bool allowCue = true) {
  if (!state) return false;

  if (!strcmp(state, "running")) {
    if (m.state == STATUS_RUNNING) return false;
    m.state = STATUS_RUNNING;
    m.since = now;
    return true;
  }

  if (!strcmp(state, "waiting")) {
    if (allowCue && m.state != STATUS_WAITING) m.pending = STATUS_EV_WAITING;
    if (m.state == STATUS_WAITING) return false;
    m.state = STATUS_WAITING;
    m.since = now;
    return true;
  }

  // done / failed
  if (allowCue && m.state != STATUS_IDLE) {
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

// When the *aggregate* state last changed. The screens use it for the "it
// started working" hold: the owner's own `since` moves whenever a different
// session takes the screen over, which would restart the hold for no reason.
inline uint32_t statusMachineSince(const StatusMachine& m) { return m.since; }

inline StatusEvent statusMachineTakeEvent(StatusMachine& m) {
  StatusEvent e = m.pending;
  m.pending = STATUS_EV_NONE;
  return e;
}
