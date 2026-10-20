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

// Three-day daily forecast (Open-Meteo), the Forecast weather card. The device
// already knows its coordinates, and Open-Meteo answers in <1 KB — wttr.in's
// JSON is ~50 KB, which is why the one-line format stays for current conditions
// and this is a different host.
struct ForecastDay {
  char   label[12];           // "Today" / "Tomorrow" / "Wed"
  char   cond[16];            // short WMO label, e.g. "Showers"
  int8_t maxC, minC;
  int8_t rainPct;             // 0..100, -1 unknown
};

struct ForecastData {
  bool valid;
  bool stale;
  ForecastDay day[3];
};

// A notice pushed over the device channel (parse/notice_parse.h), resolved
// against the clock for the Notice page: `countdownSec` is how long until the
// event starts, not how long the notice lives.
struct NoticeData {
  bool     valid;
  char     kind[12];
  char     text[64];
  bool     hasCountdown;
  uint32_t countdownSec;
};

// Which payload a fetch job targets. Kept here (not in fetch_task.h) so the
// pure poll scheduler in poller.h can name jobs without pulling in the fetch
// task's firmware-only dependencies (config.h → LittleFS/ArduinoJson). Values
// are 1-based so 0 can mean "none".
//
// New jobs are appended: the value is a diag breadcrumb and the index into
// poller.cpp's per-job arrays, and the arrays are sized from the last entry.
enum FetchJob : uint8_t {
  FETCH_USAGE = 1,
  FETCH_WEATHER,
  FETCH_MOON,
  FETCH_AIR,
  FETCH_FORECAST,
};

// The device's own state, for the Health page: everything that used to be
// visible only over USB CDC (`diag`'s periodic line, `link_probe.py`). Filled by
// main.cpp's refreshEnv() from cheap sources only — no fetch, no flash, no I2C —
// so it can ride the existing 1 Hz env tick.
//
// Deliberately *not* sourced from src/diag.*: that file is documented as
// temporary, and a permanent page must not depend on the thing that is meant to
// be deleted.
struct HealthData {
  bool     valid;             // false until the first refresh
  bool     online;            // STA associated
  int      rssi;              // dBm; downlink only (see hw/net.h)
  uint32_t uptimeSec;
  uint32_t heapInternal;      // bytes free in internal SRAM
  uint32_t heapPsram;         // bytes free in PSRAM
  bool     tokensValid;       // a usable refresh token is present
  bool     bleUp;             // the BLE fallback came up (not proof it advertises)
  int8_t   lastJob;           // FetchJob of the last finished job, 0 = none yet
  bool     lastOk;            // …and whether it succeeded
  uint32_t lastFetchAgeSec;   // seconds since that job finished
  uint32_t lastDnsMs;         // phase timings of the last request attempted
  uint32_t lastTlsMs;
  uint32_t lastReadMs;
  char     fwVersion[12];
};
