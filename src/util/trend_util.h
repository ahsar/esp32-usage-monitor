#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

// Pure cost/rate math for the trend page. Header-only and hardware-free so the
// host tests can cover it (the formatting mirrors what the card draws).
namespace trend {

// Microcents are 1e-6 cents, so $1.00 = 100 * 1e6 = 1e8.
inline uint64_t microCentsToCents(uint64_t microCents) { return microCents / 1000000ULL; }

// "$1.42" (always two decimals). Callers pass a buffer of at least 24 bytes.
inline void formatUsd(char* buf, size_t n, uint64_t microCents) {
  uint64_t cents = microCentsToCents(microCents);
  snprintf(buf, n, "$%llu.%02llu",
           (unsigned long long)(cents / 100ULL),
           (unsigned long long)(cents % 100ULL));
}

// Spend rate in microcents/hour from a delta over `dtSeconds`.
inline uint64_t burnPerHour(uint64_t deltaMicroCents, uint32_t dtSeconds) {
  if (dtSeconds == 0) return 0;
  return (uint64_t)((double)deltaMicroCents * 3600.0 / (double)dtSeconds);
}

// Seconds until `remainingMicroCents` is spent at `burn` microcents/hour.
// Returns 0 when the rate is zero (unknown), or UINT32_MAX when it is far away.
inline uint32_t etaSeconds(uint64_t remainingMicroCents, uint64_t burn) {
  if (burn == 0) return 0;
  double sec = (double)remainingMicroCents * 3600.0 / (double)burn;
  if (sec >= 4294967295.0) return 4294967295u;
  return (uint32_t)sec;
}

// "$0.42/h"
inline void formatBurn(char* buf, size_t n, uint64_t burnMicroCentsPerHour) {
  uint64_t cents = microCentsToCents(burnMicroCentsPerHour);
  snprintf(buf, n, "$%llu.%02llu/h",
           (unsigned long long)(cents / 100ULL),
           (unsigned long long)(cents % 100ULL));
}

// "2h 10m" / "45m" / "3d 4h"
inline void formatEta(char* buf, size_t n, uint32_t sec) {
  if (sec == 0) { snprintf(buf, n, "--"); return; }
  uint32_t d = sec / 86400, r = sec % 86400;
  uint32_t h = r / 3600,   m = (r % 3600) / 60;
  if (d > 0)      snprintf(buf, n, "%ud %uh", (unsigned)d, (unsigned)h);
  else if (h > 0) snprintf(buf, n, "%uh %um", (unsigned)h, (unsigned)m);
  else            snprintf(buf, n, "%um", (unsigned)m);
}

}  // namespace trend
