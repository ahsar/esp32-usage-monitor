#pragma once
// Bounded, allocation-free parsing of an HTTP request head (request line +
// headers) as it arrives from the wire. Header-only and dependency-free, so the
// firmware transport (status_server.cpp) only reads bytes and hands them here —
// and the host tests can cover the parsing without a socket.
//
// Everything takes explicit (buffer, length) pairs: a request head is untrusted
// bytes, and none of these may assume a NUL terminator before `rlen`.
#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// memmem() without depending on libc's; `pos` receives the offset on a hit.
inline bool httpMemFind(const char* hay, size_t hayLen, const char* needle,
                        size_t needleLen, size_t& pos) {
  if (needleLen == 0 || hayLen < needleLen) return false;
  for (size_t i = 0; i + needleLen <= hayLen; i++) {
    if (memcmp(hay + i, needle, needleLen) == 0) { pos = i; return true; }
  }
  return false;
}

// The query of the request line, verbatim and undecoded (no leading '?'), e.g.
// "chime=0&place=Shanghai". This exact string is what the settings and WiFi
// signatures cover, so it must be taken as bytes off the wire.
//
// `tooLong` distinguishes the two reasons this can fail — "no query at all" (a
// health probe) from "the query does not fit in `cap`" — because they deserve
// different answers: a probe gets 200, an oversized request must not be silently
// ignored. The cap is the caller's: both transports size it from
// STATUS_QUERY_MAX (status_channel.h).
inline bool httpQueryFromHead(const char* req, size_t rlen, char* out, size_t cap,
                              bool* tooLong = nullptr) {
  if (tooLong) *tooLong = false;
  size_t g;
  if (!httpMemFind(req, rlen, "GET ", 4, g)) return false;
  size_t sp = g + 4;                        // end of the request target
  while (sp < rlen && req[sp] != ' ') sp++;
  if (sp == rlen) return false;
  size_t q = g + 4;                         // '?' inside the target only
  while (q < sp && req[q] != '?') q++;
  if (q == sp) return false;
  size_t n = sp - q - 1;
  if (n == 0) return false;
  if (n + 1 > cap) {
    if (tooLong) *tooLong = true;
    return false;
  }
  memcpy(out, req + q + 1, n);
  out[n] = '\0';
  return true;
}

// Case-insensitive header lookup over the raw request head. Lines are
// newline-separated in `req` (each keeps its trailing '\r'); a stray NUL byte on
// the wire just ends matching early, and the request degrades to a health probe
// or a 405.
inline bool httpHeaderValue(const char* req, size_t rlen, const char* name,
                            char* out, size_t outLen) {
  char needle[40];
  snprintf(needle, sizeof(needle), "\n%s:", name);
  const size_t nlen = strlen(needle);
  if (rlen < nlen) return false;

  for (size_t i = 0; i + nlen <= rlen; i++) {
    size_t j = 0;
    for (; j < nlen; j++) {
      if (tolower((unsigned char)req[i + j]) != tolower((unsigned char)needle[j])) break;
    }
    if (j != nlen) continue;

    size_t v = i + nlen;
    while (v < rlen && (req[v] == ' ' || req[v] == '\t')) v++;
    size_t e = v;
    while (e < rlen && req[e] != '\r' && req[e] != '\n') e++;
    size_t len = e - v;
    if (len >= outLen) len = outLen - 1;
    memcpy(out, req + v, len);
    out[len] = 0;
    return true;
  }
  return false;
}
