#pragma once
// Dependency-free location helpers: build the request paths that carry the
// device's configured place, and format a UTC offset the way the APIs want. No
// Arduino headers, so this is compiled and unit-tested on the host
// (test/test_util). The percent-encoding itself lives in url_util.h.
#include <stddef.h>
#include <stdio.h>
#include "util/url_util.h"

// Minutes east of UTC -> the `offset=` value MET Norway expects ("+08:00"),
// URL-encoded because an unencoded '+' means "space" in a query string. The
// offset is clamped to ±16 h, the widest real-world zone. Returns false when the
// buffer is too small (9 bytes is enough).
inline bool formatTzOffsetQuery(int offsetMin, char* out, size_t cap) {
  if (!out || cap < 9) return false;
  const int MAX = 16 * 60;
  if (offsetMin > MAX)  offsetMin = MAX;
  if (offsetMin < -MAX) offsetMin = -MAX;
  int mag = offsetMin < 0 ? -offsetMin : offsetMin;
  int n = snprintf(out, cap, "%s%02d:%02d", offsetMin < 0 ? "-" : "%2B",
                   mag / 60, mag % 60);
  return n > 0 && (size_t)n < cap;
}

// wttr.in one-line path for `place` — the same nine fields weatherParseLine()
// expects, in the same order:
//   location | condition | temp | feels | humidity | wind | precip | rise | set
inline bool wttrOneLinePath(const char* place, char* out, size_t cap) {
  if (!place || !place[0]) return false;
  // Worst case for place[48] (47 chars) is 47 * "%20" = 141, plus the 10-char
  // "?m&format=" and the 26-char field list, plus the leading "/" (178).
  char enc[144];
  if (!urlEncode(place, enc, sizeof(enc))) return false;
  int n = snprintf(out, cap, "/%s?m&format=%s", enc,
                   "%l|%C|%t|%f|%h|%w|%p|%S|%s");
  return n > 0 && (size_t)n < cap;
}

// MET Norway sunrise/moon path. `lat`/`lon` are what the moonrise/moonset are
// computed for; `offset` is the local UTC offset, which is what makes the times
// come back in local minutes.
inline bool moonApiPath(double lat, double lon, int tzOffsetMin, char* out, size_t cap) {
  char off[16];
  if (!formatTzOffsetQuery(tzOffsetMin, off, sizeof(off))) return false;
  int n = snprintf(out, cap,
                   "/weatherapi/sunrise/3.0/moon?lat=%.4f&lon=%.4f&offset=%s",
                   lat, lon, off);
  return n > 0 && (size_t)n < cap;
}
