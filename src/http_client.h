#pragma once
#include <Arduino.h>

struct HttpResponse {
  int    status;
  String body;
  String date;   // raw Date header value (used as a clock fallback)
};

// Blocking HTTP(S) request over TLS (setInsecure). `extraHeaders` is raw header
// lines terminated with "\r\n" (no trailing blank line). While reading, the
// local status server is kept responsive via handleHttpStatus().
//
// http10: send "HTTP/1.0" so servers reply without chunked encoding (wttr.in
//         chunks HTTP/1.1 responses; HTTP/1.0 delimits by closing).
// maxBody: safety cap on the buffered body size.
bool httpRequest(const char* host, const char* method, const char* path,
                 const char* extraHeaders, const char* body,
                 HttpResponse& res, bool http10 = false, size_t maxBody = 20000,
                 uint32_t timeoutMs = 25000,
                 const char* userAgent = "esp32-usage-monitor");
