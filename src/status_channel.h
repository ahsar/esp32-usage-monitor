#pragma once
// The device channel, transport-neutral: the state pings and settings requests
// that used to be HTTP-only, plus WiFi credentials. Two transports carry it and
// this is the seam between them:
//
//   - status_server.cpp — plain HTTP on port 80 (the original path, and still
//     the one the OpenCode plugin prefers);
//   - ble_status.cpp    — a GATT service, for when the PC and the device cannot
//     reach each other over the LAN at all.
//
// Framing is the transport's business; the *meaning* of a request, the HMAC
// check, the settings validation and the state machine are not duplicated. That
// is why an untrusted write over BLE is exactly as safe as one over HTTP: it is
// the same verification, not a second implementation of it. All of that lives in
// status_channel.cpp; a transport only pulls the query and the HMAC pair out of
// its wire format and hands them to statusChannelHandle().
//
// Only the PC→device direction is covered. usage/weather/moon/air are fetches
// the device makes out to the internet and still need WiFi.
//
// Three request kinds travel over both transports:
//
//   - `?state=…`          — the agent-activity animation (main.cpp);
//   - everything else     — the settings channel (settings_parse.h), a signed
//                           set of config overrides;
//   - `?wifi_ssid=…&wifi_pass=…` — WiFi credentials (wifi_cred_parse.h). Its own
//                           kind rather than a settings field: it is a secret, it
//                           is *not* persisted in /settings.json, and it is the
//                           one request that has to work on a device with no IP
//                           address yet — i.e. over BLE, which is why the LAN
//                           transport being absent must not make it fail.
//
// A request is the query string the client wants applied plus the HMAC pair it
// signed that string with. The payload the signature covers depends on the
// request — the state value for `?state=`, the whole query for settings and for
// WiFi credentials — and that rule lives in statusChannelHandle(), not in either
// transport.
#include <stddef.h>
#include <stdint.h>

#include "parse/settings_parse.h"
#include "parse/status_machine.h"
#include "parse/wifi_cred_parse.h"

// The longest query either transport accepts. A WiFi credentials request with
// every byte percent-encoded is the worst case: 32-byte SSID + 63-byte
// passphrase, each ×3, plus the key names. Both transports size their buffers
// from these, so the two framings cannot drift apart.
constexpr size_t STATUS_QUERY_MAX = 384;
constexpr size_t STATUS_BODY_MAX  = 512;
constexpr size_t STATUS_TS_MAX    = 24;    // unix seconds; 10 digits is 2286
constexpr size_t STATUS_SIG_MAX   = 80;    // 64 hex chars + slack

struct StatusReq {
  // Raw, undecoded query without the leading '?': "state=running",
  // "chime=0&place=Shanghai", or "settings". For a settings request these bytes
  // are what the signature covers, so they must arrive verbatim (a frame that
  // re-encoded or re-ordered them would simply fail auth).
  const char* query;
  const char* ts;    // X-Ts: client unix seconds (ignored when auth is off)
  const char* sig;   // X-Sig: hex HMAC-SHA256 over "<payload>|<ts>"
  const char* peer;  // IP or BT address; used for the rejection log only
};

// Handle one request, writing the response text into `body`, and return the
// status line ("200 OK", "401 Unauthorized", …) from static storage. A short
// answer is normal: the body is the settings summary for a settings request and
// "OK" otherwise.
//
// Call it on the loop task only. A settings request persists through the
// callback registered with statusChannelSetSettingsCallback(), and that write is
// flash — never the fetch task, never the BT stack's own task.
const char* statusChannelHandle(const StatusReq& req, char* body, size_t bodyCap);

// ---------------------------------------------------------------------------
// Channel state / configuration (implemented in status_channel.cpp)
// ---------------------------------------------------------------------------

// Shared secret for request auth. Empty string (or nullptr) disables auth.
void statusChannelSetKey(const char* key);
bool statusChannelAuthEnabled();

// Called on the transition into running/waiting (e.g. to wake the screen).
void statusChannelSetWakeCallback(void (*cb)());

// Seed the settings channel with what boot loaded from /settings.json, so
// `?settings` answers with the truth from the first request on.
void statusChannelSetSettings(const DeviceSettings& s);

// Called with the overrides a signed settings request produced and the fields it
// touched. Implemented in main.cpp: stores them (/settings.json) *and* applies
// them live. Returning false means the change was rejected, and the device keeps
// answering with the previous settings.
void statusChannelSetSettingsCallback(bool (*cb)(const DeviceSettings&, uint16_t changed));

// Called with the credentials (or the forget request) a signed `?wifi_ssid=…`
// request produced. Implemented in main.cpp, which stores them in NVS — never in
// /settings.json, and never touched from here. Returning false means the write
// failed, so the request is answered with an error rather than a claim.
//
// The reply can only ever report that the request was *recorded*: the passphrase
// reaches NVS once the association succeeds, which is after this returns (and
// after the reply is out). A callback that returns true and an association that
// then fails is a normal outcome, not an error — the panel and the log are what
// report it (docs/pc-bridge.md).
//
// The callback must not touch the radio: it runs inside the request handler on
// the loop task, and associating (or dropping the link an HTTP reply just
// travelled over) has to happen after that reply is out. main.cpp only records
// the request here and acts on it at the bottom of the loop.
void statusChannelSetWifiCallback(bool (*cb)(const WifiCred&));

// True while the agent owns the screen (running or waiting).
bool     statusChannelThinking();
// True only in the waiting (needs-you) state.
bool     statusChannelWaiting();
uint32_t statusChannelThinkingSince();
// Drop back to idle without producing an event (used by Key1).
void     statusChannelClearThinking();

// One-shot completion events, consumed by loop() to fire the right chime.
// statusChannelClearThinking() never produces one, so dismissing the animation
// by hand stays silent.
StatusEvent statusChannelTakeEvent();
