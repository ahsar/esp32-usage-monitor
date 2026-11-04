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
//
// The last three are the "every session edge" cues and are only *played* when
// `chime_edges` is on (main.cpp): working is the ambient state, so a start is
// silent by default, and so is a completion that leaves another session running.
// The machine still queues them — the policy belongs to the consumer, not here —
// which keeps this file free of config.
enum StatusEvent : uint8_t {
  STATUS_EV_NONE = 0,
  STATUS_EV_DONE,
  STATUS_EV_FAILED,
  STATUS_EV_WAITING,
  STATUS_EV_START,           // a session started (idle→running, waiting→running)
  STATUS_EV_SESSION_DONE,    // one session ended while others still run
  STATUS_EV_SESSION_FAILED,  // …and it failed
};

struct StatusMachine {
  StatusState state = STATUS_IDLE;
  uint32_t    since = 0;                 // millis() of the last state change
  StatusEvent pending = STATUS_EV_NONE;  // not yet consumed by loop()
};

// Higher wins. `pending` is a single slot consumed once per loop() iteration, but
// one iteration can see several edges (the last session finishing also queues a
// session-done, a completion and a start can land in the same request burst), so
// the slot keeps the most actionable event instead of the last write.
//
// Above the session edges sit the two facts the user must not miss (a question,
// and the aggregate leaving active), and above those the failure. START is lowest
// on purpose: it is the one event the consumer may drop (`chime_edges` off), and a
// cue about to be discarded must never mask one that would have been played.
inline int statusEventRank(StatusEvent ev) {
  switch (ev) {
    case STATUS_EV_FAILED:         return 5;
    case STATUS_EV_DONE:           return 4;
    case STATUS_EV_WAITING:        return 3;
    case STATUS_EV_SESSION_FAILED: return 2;
    case STATUS_EV_SESSION_DONE:   return 1;
    case STATUS_EV_START:          return 0;
    default:                       return -1;
  }
}

inline void statusMachineQueue(StatusMachine& m, StatusEvent ev) {
  if (ev == STATUS_EV_NONE) return;
  if (statusEventRank(ev) >= statusEventRank(m.pending)) m.pending = ev;
}

// Apply one *authenticated* state ping. Returns true when the state changed, so
// the caller can wake the panel.
//
//   running       → active, queueing the start cue; a duplicate running is not a
//                   transition (and a heartbeat must stay silent)
//   waiting       → active, and queues the needs-you chime on the way in
//   done/failed   → idle, queuing a completion event only if it was active
//
// The start cue is queued unconditionally; whether it is *played* is
// `chime_edges`, decided in main.cpp. A session that starts while another one is
// already running never reaches this function with a different aggregate (see
// agent_sessions.h), which is why status_channel.cpp queues that edge itself.
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
    if (allowCue) statusMachineQueue(m, STATUS_EV_START);
    m.state = STATUS_RUNNING;
    m.since = now;
    return true;
  }

  if (!strcmp(state, "waiting")) {
    if (allowCue && m.state != STATUS_WAITING) statusMachineQueue(m, STATUS_EV_WAITING);
    if (m.state == STATUS_WAITING) return false;
    m.state = STATUS_WAITING;
    m.since = now;
    return true;
  }

  // done / failed
  if (allowCue && m.state != STATUS_IDLE) {
    statusMachineQueue(m, strcmp(state, "failed") ? STATUS_EV_DONE : STATUS_EV_FAILED);
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
