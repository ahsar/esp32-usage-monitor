#pragma once
#include <Arduino.h>

struct HttpResponse {
  int    status;
  String body;
  String date;   // raw Date header value (used as a clock fallback)
};

// Blocking HTTP(S) request over TLS with chain verification against the ESP-IDF
// CA bundle. `extraHeaders` is raw header lines terminated with "\r\n" (no
// trailing blank line).
//
// Fully blocking — DNS + TCP + TLS connect has no callback to pump from — so
// this must never run on the main loop task. All callers go through the
// background worker in fetch_task.cpp; loop() only schedules and harvests.
//
// Every blocking phase is bounded and logged ("http: <host> -> <status>
// dns=… tls=… read=…"): the name is resolved with its own deadline first
// (TLS connect no longer hides DNS), the handshake is capped, and the read
// loop runs for at most `timeoutMs`. The whole request therefore has a worst
// case of ~8 s + 10 s + 15 s + `timeoutMs`, so a blocked host can stall the
// worker but never wedge it.
//
// http10: send "HTTP/1.0" so servers reply without chunked encoding (wttr.in
//         chunks HTTP/1.1 responses; HTTP/1.0 delimits by closing).
// maxBody: safety cap on the buffered body size.
bool httpRequest(const char* host, const char* method, const char* path,
                 const char* extraHeaders, const char* body,
                 HttpResponse& res, bool http10 = false, size_t maxBody = 20000,
                 uint32_t timeoutMs = 25000,
                 const char* userAgent = "esp32-usage-monitor");

// True when the *last* httpRequest() failed at the transport layer (DNS, the
// TCP/TLS connect, or "no response headers at all") instead of getting an HTTP
// status back. Nothing about the server is known in that case, but it does say
// the path carried nothing: the poller policy uses it to tell "this AP is dead"
// (re-select an AP) from "the server said no" (a 5xx proves the link works, and
// roaming would be pointless). Reset at the top of every request, so it always
// describes the most recent one.
bool httpTransportFailed();
