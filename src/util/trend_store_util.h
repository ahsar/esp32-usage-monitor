#pragma once
// One line of the trend file, and its parse — split out so the format is
// host-tested while the LittleFS I/O stays in trend_store.cpp.
//
// The file is a plain-text log, one sample per line:
//
//   esp32-trend 1
//   1791436863,320000000,45
//
// Text rather than a packed struct on purpose: the ring is ~120 points, so the
// size difference does not matter, and a truncated tail (power lost mid-write)
// costs the last line instead of the whole file. `epoch` is wall-clock seconds —
// the in-RAM ring uses boot-relative milliseconds, which are meaningless after a
// reboot, so this is the one field that makes the history survive one.
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

constexpr const char* TREND_STORE_MAGIC = "esp32-trend 1";

// Upper bound on samples the file may restore: the ring is smaller than this,
// and the cap keeps a hand-edited or corrupt file from growing the read loop.
constexpr int TREND_STORE_MAX = 240;

struct TrendRecord {
  uint32_t epoch;             // wall clock at the sample
  uint64_t usedMicroCents;    // rolling-window spend
  uint8_t  pct;               // rolling-window percent
};

// "epoch,used,pct" into `out`. Returns the length, or 0 when it does not fit.
inline int trendStoreLine(const TrendRecord& r, char* out, size_t cap) {
  int n = snprintf(out, cap, "%lu,%llu,%u", (unsigned long)r.epoch,
                   (unsigned long long)r.usedMicroCents, (unsigned)r.pct);
  return (n > 0 && (size_t)n < cap) ? n : 0;
}

// Parse one line. Strict enough that a truncated or foreign line is rejected
// (the caller then stops at that point and keeps what it already has).
inline bool trendStoreParseLine(const char* line, TrendRecord& out) {
  if (!line || !line[0]) return false;
  unsigned long epoch = 0, pct = 0;
  unsigned long long used = 0;
  if (sscanf(line, "%lu,%llu,%lu", &epoch, &used, &pct) != 3) return false;
  if (pct > 100) return false;
  if (epoch == 0) return false;          // a sample with no wall clock is useless
  out.epoch          = (uint32_t)epoch;
  out.usedMicroCents = (uint64_t)used;
  out.pct            = (uint8_t)pct;
  return true;
}
