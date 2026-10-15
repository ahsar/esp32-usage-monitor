#pragma once
#include <stdint.h>

// Tiny HTTP status server on port 80. The host (or an editor plugin) pings it
// with `?state=running|waiting|done|failed` to reflect agent activity on the
// panel:
//   running → agent is working          → "Thinking..." animation
//   waiting → agent needs your input    → "Needs you" animation
//   done    → finished                  → chime (rising)
//   failed  → errored                   → chime (falling)
//
// The state lives here because the panel render must react to it from wherever
// it runs; the server itself is only ever pumped from loop().
//
// Auth: the endpoint is reachable by anyone on the LAN, so when a shared key is
// configured every request must carry an HMAC-SHA256 signature over
// "<state>|<unixSeconds>" in the `X-Sig` / `X-Ts` headers. The transport stays
// plain HTTP (no TLS), but the signature makes requests unforgeable, and the
// timestamp window makes them unreplayable. With no key configured, auth is
// DISABLED and the server logs a warning at boot.
//
// Note: the HTTPS fetches run on the background fetch task (fetch_task.cpp),
// which must never call this — s_wakeCb() touches main-task UI state.

void statusServerBegin();

// Shared secret for request auth. Empty string (or nullptr) disables auth.
void statusServerSetKey(const char* key);
bool statusServerAuthEnabled();

// Called on the transition into running/waiting (e.g. to wake the screen).
void statusServerSetWakeCallback(void (*cb)());

// True while the agent owns the screen (running or waiting).
bool     statusServerThinking();
// True only in the waiting (needs-you) state.
bool     statusServerWaiting();
uint32_t statusServerThinkingSince();
// Drop back to idle without producing an event (used by Key1).
void     statusServerClearThinking();

// One-shot completion events, consumed by loop() to fire the right chime.
// statusServerClearThinking() never produces one, so dismissing the animation
// by hand stays silent.
enum StatusEvent : uint8_t {
  STATUS_EV_NONE = 0,
  STATUS_EV_DONE,
  STATUS_EV_FAILED,
  STATUS_EV_WAITING,
};
StatusEvent statusServerTakeEvent();

// Implemented here; called from the top of loop() every iteration.
extern "C" void handleHttpStatus();
