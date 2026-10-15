#pragma once
#include <stdint.h>
#include "parse/settings_parse.h"

// Tiny HTTP status server on port 80. The host (or an editor plugin) pings it
// with `?state=running|waiting|done|failed` to reflect agent activity on the
// panel:
//   running → agent is working          → "Thinking..." animation
//   waiting → agent needs your input    → "Needs you" animation
//   done    → finished                  → chime (rising)
//   failed  → errored                   → chime (falling)
//
// The same server also carries the **settings channel**, so the knobs you would
// otherwise have to edit into config.json and `uploadfs` (which erases the
// device's rotated OAuth token) can be changed over the network:
//
//   ?chime=0&brightness=4        set overrides (see settings_parse.h for keys)
//   ?settings                    read them back
//   ?place=Shanghai&lat=31.2304&lon=121.4737&tz_offset_min=480
//
// A settings request is signed like a state one, but over the **raw query string**
// ("chime=0&brightness=4|<ts>") rather than a single value, so every value in the
// request is covered by the signature and the client needs no canonicalisation.
// `tools/status-settings.sh` does exactly that. Settings are refused outright
// (403) when no `status_key` is configured: unlike an animation, they persist
// device configuration, so there is no unauthenticated write path.
//
// The state lives here because the panel render must react to it from wherever
// it runs; the server itself is only ever pumped from loop().
//
// Auth: the endpoint is reachable by anyone on the LAN, so when a shared key is
// configured every request must carry an HMAC-SHA256 signature in the `X-Sig` /
// `X-Ts` headers. The transport stays plain HTTP (no TLS), but the signature
// makes requests unforgeable, and the timestamp window makes them unreplayable.
// With no key configured, auth is DISABLED and the server logs a warning at boot
// (state requests still work; settings do not).
//
// Note: the HTTPS fetches run on the background fetch task (fetch_task.cpp),
// which must never call this — s_wakeCb() touches main-task UI state.

void statusServerBegin();

// Shared secret for request auth. Empty string (or nullptr) disables auth.
void statusServerSetKey(const char* key);
bool statusServerAuthEnabled();

// Called on the transition into running/waiting (e.g. to wake the screen).
void statusServerSetWakeCallback(void (*cb)());

// Seed the settings channel with what boot loaded from /settings.json, so
// `?settings` answers with the truth from the first request on.
void statusServerSetSettings(const DeviceSettings& s);

// Called with the overrides a signed settings request produced and the fields it
// touched. Implemented in main.cpp: stores them (/settings.json) *and* applies
// them live. Returning false means the change was rejected, and the device keeps
// answering with the previous settings.
void statusServerSetSettingsCallback(bool (*cb)(const DeviceSettings&, uint16_t changed));

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
