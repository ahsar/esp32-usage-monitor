#pragma once
#include <stddef.h>

// The device channel, transport-neutral: the state pings and settings requests
// that used to be HTTP-only. Two transports carry it and this is the seam
// between them:
//
//   - status_server.cpp — plain HTTP on port 80 (the original path, and still
//     the one the OpenCode plugin prefers);
//   - ble_status.cpp    — a GATT service, for when the PC and the device cannot
//     reach each other over the LAN at all.
//
// Framing is the transport's business; the *meaning* of a request, the HMAC
// check, the settings validation and the state machine are not duplicated. That
// is why an untrusted write over BLE is exactly as safe as one over HTTP: it is
// the same verification, not a second implementation of it.
//
// Only the PC→device direction is covered. usage/weather/moon/air are fetches
// the device makes out to the internet and still need WiFi.
//
// A request is the query string the client wants applied plus the HMAC pair it
// signed that string with. The payload the signature covers depends on the
// request — the state value for `?state=`, the whole query for settings — and
// that rule lives in statusChannelHandle(), not in either transport.
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
// callback registered with statusServerSetSettingsCallback(), and that write is
// flash — never the fetch task, never the BT stack's own task.
const char* statusChannelHandle(const StatusReq& req, char* body, size_t bodyCap);
