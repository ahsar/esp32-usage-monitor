#pragma once
// Dependency-free URL helpers: percent-encoding for the request paths the
// clients build, and decoding for the values the settings channel reads. No
// Arduino headers, so this is unit-tested on the host (test/test_util).
#include <stddef.h>
#include <stdio.h>

static inline int urlHexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Percent-encode `src` for use in a path segment or query value. The unreserved
// set (ALPHA / DIGIT / "-._~") passes through; everything else becomes %XX.
// Returns false when the encoded result (plus its NUL) does not fit in `cap`.
inline bool urlEncode(const char* src, char* out, size_t cap) {
  if (!out || cap == 0) return false;
  size_t n = 0;
  for (const char* p = src; p && *p; p++) {
    unsigned char c = (unsigned char)*p;
    bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
    if (safe) {
      if (n + 1 >= cap) return false;
      out[n++] = (char)c;
    } else {
      if (n + 3 >= cap) return false;
      snprintf(out + n, 4, "%%%02X", c);
      n += 3;
    }
  }
  out[n] = '\0';
  return true;
}

// Decode `src` into `out`: %XX -> byte, and '+' -> ' ' (the query-string spelling
// of a space, which is what a hand-typed `place=New+York` uses). A malformed
// escape is passed through literally rather than failing the whole request — the
// settings parser validates the value that comes out. Returns false only when the
// decoded result does not fit in `cap`.
inline bool urlDecode(const char* src, char* out, size_t cap) {
  if (!out || cap == 0) return false;
  size_t n = 0;
  for (const char* p = src; p && *p; p++) {
    unsigned char c = (unsigned char)*p;
    if (c == '%' && p[1] && p[2]) {
      int hi = urlHexVal(p[1]), lo = urlHexVal(p[2]);
      if (hi >= 0 && lo >= 0) {
        c = (unsigned char)((hi << 4) | lo);
        p += 2;
      }
    } else if (c == '+') {
      c = ' ';
    }
    if (n + 1 >= cap) return false;
    out[n++] = (char)c;
  }
  out[n] = '\0';
  return true;
}
