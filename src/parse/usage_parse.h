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
#include "api_client.h"
#include "util/time_util.h"

namespace usage_parse_detail {

inline unsigned long long jsonU64(JsonVariantConst v) {
  if (v.is<const char*>()) {
    const char* s = v.as<const char*>();
    return s ? strtoull(s, nullptr, 10) : 0;
  }
  if (v.is<long long>())          return (unsigned long long)v.as<long long>();
  if (v.is<unsigned long long>()) return v.as<unsigned long long>();
  if (v.is<double>())             return (unsigned long long)v.as<double>();
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
