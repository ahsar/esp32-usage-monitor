#pragma once
// Plain data structs shared by the network clients, the header-only parsers and
// the UI.
//
// They live in their own dependency-free header so the UI layer and the parsers
// do not have to include the network client headers — those drag in the OAuth
// token types (token_store.h) and nothing in the UI path needs them. The
// clients include THIS header, never the reverse.
#include <stdint.h>

// Per-meter spend, added so the trend page can show cost and its rate rather
// than just a percentage. Microcents are 1e-6 cents, so $1.00 = 1e8.
struct UsageData {
  bool valid;
  int rollingPercent;
  uint32_t rollingResetSec;
  int weeklyPercent;
  uint32_t weeklyResetSec;
  int monthlyPercent;
  uint32_t monthlyResetSec;

  uint64_t rollingUsedMicroCents;
  uint64_t rollingLimitMicroCents;
  uint64_t weeklyUsedMicroCents;
  uint64_t weeklyLimitMicroCents;
  uint64_t monthlyUsedMicroCents;
  uint64_t monthlyLimitMicroCents;
};

// Current conditions, parsed from wttr.in's one-line format endpoint.
struct WeatherData {
  bool valid;
  bool stale;
  int  tempC;
  int  feelsC;
  int  humidity;
  int  windKmph;
  char windDir[8];            // "N".."NW" derived from the wind arrow
  int  precipTenths;          // mm * 10
  int  sunriseMin;            // minutes since local midnight, -1 unknown
  int  sunsetMin;
  char condition[40];
  char area[32];
  char obsTime[12];           // local "HH:MM" stamped by the caller
};

// Moon data, parsed from MET Norway's sunrise/moon API.
struct MoonData {
  bool valid;
  bool stale;
  int  moonriseMin;           // minutes since local midnight, -1 unknown
  int  moonsetMin;
  int  illumPct;              // 0..100
  bool waxing;
  char phaseName[24];
};

// Air quality from the World Air Quality Index (aqicn/waqi) station feed.
struct AirData {
  bool valid;
  bool stale;
  int  aqi;                   // -1 when unknown
  int  pm25;                  // -1 when unknown
  int  pm10;                  // -1 when unknown
  char observedAt[8];         // "14:00"
};

// Which payload a fetch job targets. Kept here (not in fetch_task.h) so the
// pure poll scheduler in poller.h can name jobs without pulling in the fetch
// task's firmware-only dependencies (config.h → LittleFS/ArduinoJson). Values
// are 1-based so 0 can mean "none".
enum FetchJob : uint8_t {
  FETCH_USAGE = 1,
  FETCH_WEATHER,
  FETCH_MOON,
  FETCH_AIR,
};
