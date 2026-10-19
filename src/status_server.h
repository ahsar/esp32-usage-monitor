#pragma once
// The HTTP transport for the device channel: a tiny server on port 80 that
// accepts one connection per loop() iteration, reads a bounded request head,
// frames the query + HMAC pair and calls statusChannelHandle().
//
// The channel itself — auth, the state machine, settings and WiFi dispatch —
// lives in status_channel.{h,cpp}, not here. See that header for the protocol.
//
// Auth: the endpoint is reachable by anyone on the LAN, so when a shared key is
// configured every request must carry an HMAC-SHA256 signature in the `X-Sig` /
// `X-Ts` headers. The transport stays plain HTTP (no TLS), but the signature
// makes requests unforgeable, and the timestamp window makes them unreplayable.
// With no key configured, auth is DISABLED and status_channel.cpp logs a warning
// at boot (state requests still work; settings do not).
//
// Note: the HTTPS fetches run on the background fetch task (fetch_task.cpp),
// which must never call this — the wake callback touches main-task UI state.

void statusServerBegin();

// Implemented here; called from the top of loop() every iteration.
extern "C" void handleHttpStatus();
