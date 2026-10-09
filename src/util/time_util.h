#pragma once
// Dependency-free date/time parsing helpers. No Arduino headers, so this can be
// compiled and unit-tested on the host (see test/test_util).
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Floor for a "synced" clock (2020-09-13 UTC). Below this, time() is treated as
// unset and callers fall back to a parsed Date header / RTC.
constexpr uint32_t CLOCK_VALID_MIN = 1600000000u;

// Days-from-civil algorithm (Howard Hinnant), returns UTC epoch seconds.
inline uint32_t civilToEpoch(int Y, int Mo, int D, int H, int Mi, int S) {  int y = Y - (Mo <= 2);
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153u * (unsigned)(Mo + (Mo > 2 ? -3 : 9)) + 2) / 5 + (unsigned)D - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long long days = (long long)era * 146097 + (long long)doe - 719468;
  long long secs = days * 86400LL + H * 3600 + Mi * 60 + S;
  return (uint32_t)secs;
}

// "2026-10-08T05:21:03..." -> UTC epoch seconds (0 on failure).
inline uint32_t isoToEpoch(const char* iso) {
  int Y, Mo, D, H, Mi, S;
  if (!iso || sscanf(iso, "%d-%d-%dT%d:%d:%d", &Y, &Mo, &D, &H, &Mi, &S) != 6) return 0;
  return civilToEpoch(Y, Mo, D, H, Mi, S);
}

// "Thu, 08 Oct 2026 05:21:03 GMT" -> UTC epoch seconds (0 on failure).
inline uint32_t httpDateToEpoch(const char* d) {
  if (!d) return 0;
  int day, year, h, mi, s;
  char mon[8] = {0};
  if (sscanf(d, "%*3s, %d %7s %d %d:%d:%d", &day, mon, &year, &h, &mi, &s) != 6) return 0;
  static const char* MONTHS = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char* p = strstr(MONTHS, mon);
  if (!p) return 0;
  int mo = (int)((p - MONTHS) / 3) + 1;
  return civilToEpoch(year, mo, day, h, mi, s);
}

// "06:16:52" or "06:16" -> minutes since midnight (-1 on failure).
inline int parseHms(const char* s) {
  if (!s || !s[0]) return -1;
  int h = 0, m = 0;
  if (sscanf(s, "%d:%d", &h, &m) < 2) return -1;
  return h * 60 + m;
}

// "2026-10-08T03:27+08:00" -> local minutes since midnight (-1 on failure).
inline int parseIsoLocalMinutes(const char* s) {
  if (!s) return -1;
  const char* t = strchr(s, 'T');
  if (!t) return -1;
  return parseHms(t + 1);
}
