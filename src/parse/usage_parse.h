#pragma once
// Pure parsing for the opencode console usage payload.
//
// Header-only and hardware-free (only ArduinoJson + the util helpers), so the
// firmware calls it from api_client.cpp and the host unit tests compile it
// directly (test/test_util). No network / Arduino types leak in here.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ArduinoJson.h>
#include "data_types.h"
#include "util/time_util.h"

namespace usage_parse_detail {

inline unsigned long long jsonU64(JsonVariantConst v) {
  // The meter fields are counters, never negative. A "-5" handed to strtoull
  // silently wraps to ~1.8e19 and flows on as an astronomic percent and a
  // multi-trillion-dollar spend, so anything signed is treated as a server bug
  // and read as 0 (the absent-value answer), not as a number.
  if (v.is<const char*>()) {
    const char* s = v.as<const char*>();
    if (!s) return 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') return 0;
    return strtoull(s, nullptr, 10);
  }
  if (v.is<long long>()) {
    long long n = v.as<long long>();
    return n < 0 ? 0 : (unsigned long long)n;
  }
  if (v.is<unsigned long long>()) return v.as<unsigned long long>();
  if (v.is<double>()) {
    double d = v.as<double>();
    return d < 0 ? 0 : (unsigned long long)d;
  }
  return 0;
}

inline int meterPercent(JsonVariantConst meter) {
  unsigned long long used  = jsonU64(meter["usedMicroCents"]);
  unsigned long long limit = jsonU64(meter["limitMicroCents"]);
  if (limit == 0) return 0;
  unsigned long long pct = (used * 100ULL) / limit;
  if (pct > 100) pct = 100;
  return (int)pct;
}

}  // namespace usage_parse_detail
// Parse the /console/api/go/status body into `out`.
// `nowSec` is the current epoch (or 0 / below CLOCK_VALID_MIN when the clock is
// unset); it is only used to turn each meter's absolute resetsAt into a delta.
inline bool usageParse(const char* body, uint32_t nowSec, UsageData& out) {
  out = UsageData{};   // also zeroes the fields a missing meter won't overwrite

  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) return false;

  JsonObjectConst meters = doc["access"]["meters"].as<JsonObjectConst>();
  if (meters.isNull()) return false;

  struct MeterMap { const char* key; int* pct; uint32_t* reset; uint64_t* used; uint64_t* limit; };
  MeterMap map[3] = {
    { "fiveHour", &out.rollingPercent, &out.rollingResetSec,
      &out.rollingUsedMicroCents, &out.rollingLimitMicroCents },
    { "week",     &out.weeklyPercent,  &out.weeklyResetSec,
      &out.weeklyUsedMicroCents,  &out.weeklyLimitMicroCents  },
    { "month",    &out.monthlyPercent, &out.monthlyResetSec,
      &out.monthlyUsedMicroCents, &out.monthlyLimitMicroCents },
  };

  bool any = false;
  for (auto& m : map) {
    JsonObjectConst meter = meters[m.key].as<JsonObjectConst>();
    if (meter.isNull()) continue;

    *m.pct   = usage_parse_detail::meterPercent(meter);
    *m.used  = usage_parse_detail::jsonU64(meter["usedMicroCents"]);
    *m.limit = usage_parse_detail::jsonU64(meter["limitMicroCents"]);

    uint32_t resetsAt = isoToEpoch(meter["resetsAt"] | "");
    *m.reset = (resetsAt && nowSec >= CLOCK_VALID_MIN && resetsAt > nowSec)
                   ? (resetsAt - nowSec) : 0;
    any = true;
  }

  out.valid = any;
  return any;
}
