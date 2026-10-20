#pragma once
// Pure parsing for the weather / moon / air payloads.
//
// Header-only and hardware-free (ArduinoJson + util helpers only) so the
// firmware calls it from weather_client.cpp and the host unit tests compile it
// directly. No network or Arduino types leak in here.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ArduinoJson.h>
#include "data_types.h"
#include "util/time_util.h"
#include "util/value_util.h"

namespace weather_parse_detail {

constexpr float kPi = 3.14159265358979323846f;

inline int iaqi(JsonVariantConst iaqiObj, const char* key) {
  JsonVariantConst v = iaqiObj[key];
  if (v.isNull()) return -1;
  JsonVariantConst val = v["v"];
  if (val.isNull()) return -1;
  if (val.is<const char*>()) {
    const char* s = val.as<const char*>();
    return (s && s[0]) ? atoi(s) : -1;
  }
  if (val.is<float>() || val.is<double>()) return (int)(val.as<double>() + 0.5);
  return val.as<int>();
}

inline bool isSpace(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Bounded string copy (avoids strlcpy, which is not portable to the host).
inline void copyStr(char* dst, size_t cap, const char* src) {
  if (cap == 0) return;
  size_t i = 0;
  if (src) {
    for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
  }
  dst[i] = 0;
}

// wttr.in wind arrow (UTF-8 e2 86 9x) -> 8-point compass string.
inline void windDirFromArrow(const char* w, char* out, size_t outSz) {
  static const char* DIRS[8] = { "S", "SW", "W", "NW", "N", "NE", "E", "SE" };
  const unsigned char* p = (const unsigned char*)w;
  int idx = 0;
  while (p && *p) {
    if (p[0] == 0xE2 && p[1] == 0x86) {
      switch (p[2]) {
        case 0x96: idx = 3; break;   // NW  up-left
        case 0x97: idx = 5; break;   // NE  up-right
        case 0x98: idx = 7; break;   // SE  down-right
        case 0x99: idx = 1; break;   // SW  down-left
        case 0x90: idx = 2; break;   // W
        case 0x91: idx = 4; break;   // N
        case 0x92: idx = 6; break;   // E
        case 0x93: idx = 0; break;   // S
        default: break;
      }
      break;
    }
    p++;
  }
  copyStr(out, outSz, DIRS[idx]);
}

// Split "a|b|c" into up to `n` fields (each trimmed); returns the count found.
inline int splitFields(const char* s, char fields[][48], int n) {
  int count = 0;
  const char* start = s ? s : "";
  for (const char* p = start;; p++) {
    if (*p == '|' || *p == '\0') {
      if (count < n) {
        const char* f = start;
        size_t len = (size_t)(p - f);
        while (len > 0 && isSpace(f[0])) { f++; len--; }
        while (len > 0 && isSpace(f[len - 1])) len--;
        if (len > 47) len = 47;
        memcpy(fields[count], f, len);
        fields[count][len] = 0;
        count++;
      }
      if (*p == '\0') break;
      start = p + 1;
    }
  }
  return count;
}

}  // namespace weather_parse_detail

// wttr.in one-line "a|b|c|..." -> WeatherData.
inline bool weatherParseLine(const char* body, WeatherData& out) {
  using namespace weather_parse_detail;
  char f[9][48];
  int n = splitFields(body, f, 9);
  if (n < 9) return false;

  copyStr(out.area, sizeof(out.area), f[0]);
  copyStr(out.condition, sizeof(out.condition), f[1]);
  out.tempC    = atoi(f[2]);
  out.feelsC   = atoi(f[3]);
  out.humidity = atoi(f[4]);
  windDirFromArrow(f[5], out.windDir, sizeof(out.windDir));
  const char* num = strpbrk(f[5], "0123456789");
  out.windKmph = atoi(num ? num : "0");
  out.precipTenths = (int)(atof(f[6]) * 10.0f + 0.5f);
  out.sunriseMin = parseHms(f[7]);
  out.sunsetMin  = parseHms(f[8]);

  out.valid = true;
  out.stale = false;
  out.obsTime[0] = 0;
  return true;
}

// MET Norway sunrise/moon JSON -> MoonData.
inline bool moonParseJson(const char* body, MoonData& out) {
  using namespace weather_parse_detail;
  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) return false;

  JsonObjectConst props = doc["properties"].as<JsonObjectConst>();
  if (props.isNull()) return false;

  out.moonriseMin = parseIsoLocalMinutes(props["moonrise"]["time"] | "");
  out.moonsetMin  = parseIsoLocalMinutes(props["moonset"]["time"] | "");

  float phase = 0.0f;
  JsonVariantConst mp = props["moonphase"];
  if (!mp.isNull()) phase = mp.as<float>();
  float p = phase / 360.0f;
  while (p < 0) p += 1.0f;
  while (p >= 1) p -= 1.0f;

  out.waxing   = (p < 0.5f);
  out.illumPct = (int)((1.0f - cosf(2.0f * kPi * p)) * 50.0f + 0.5f);
  copyStr(out.phaseName, sizeof(out.phaseName), phaseNameFor(p));

  out.valid = true;
  out.stale = false;
  return true;
}

// waqi JSON -> AirData.
inline bool airParseJson(const char* body, AirData& out) {
  using namespace weather_parse_detail;

  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) return false;

  const char* status = doc["status"] | "";
  if (strcmp(status, "ok") != 0) return false;

  JsonVariantConst data = doc["data"];

  JsonVariantConst aqi = data["aqi"];
  if (aqi.is<const char*>()) {
    const char* s = aqi.as<const char*>();
    out.aqi = (s && s[0] >= '0' && s[0] <= '9') ? atoi(s) : -1;
  } else if (aqi.isNull()) {
    out.aqi = -1;
  } else {
    out.aqi = aqi.as<int>();
  }

  JsonVariantConst iaqiObj = data["iaqi"];
  out.pm25 = iaqi(iaqiObj, "pm25");
  out.pm10 = iaqi(iaqiObj, "pm10");

  const char* ts = data["time"]["s"] | "";
  if (strlen(ts) >= 16) {
    copyStr(out.observedAt, sizeof(out.observedAt), ts + 11);
    out.observedAt[5] = 0;   // keep "HH:MM"
  } else {
    out.observedAt[0] = 0;
  }

  out.valid = true;
  out.stale = false;
  return true;
}

// WMO weather code -> the short label the Forecast card has room for.
inline const char* wmoLabel(int code) {
  switch (code) {
    case 0:  return "Clear";
    case 1:  return "Mostly clear";
    case 2:  return "Partly cloudy";
    case 3:  return "Overcast";
    case 45: case 48: return "Fog";
    case 51: case 53: case 55: case 56: case 57: return "Drizzle";
    case 61: case 63: case 65: case 66: case 67: return "Rain";
    case 71: case 73: case 75: case 77: return "Snow";
    case 80: case 81: case 82: return "Showers";
    case 85: case 86: return "Snow showers";
    case 95: case 96: case 99: return "Storm";
    default: return "--";
  }
}

// Open-Meteo daily JSON -> ForecastData (three days).
//
// Only the arrays are read; a short response keeps the days it did carry, which
// is better than discarding a whole forecast over a missing third day. A null
// `precipitation_probability_max` is "unknown" (-1), never "0% chance".
inline bool forecastParseJson(const char* body, ForecastData& out) {
  using namespace weather_parse_detail;

  out = ForecastData{};
  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) return false;

  JsonObjectConst daily = doc["daily"].as<JsonObjectConst>();
  if (daily.isNull()) return false;

  JsonArrayConst times = daily["time"].as<JsonArrayConst>();
  JsonArrayConst maxs  = daily["temperature_2m_max"].as<JsonArrayConst>();
  JsonArrayConst mins  = daily["temperature_2m_min"].as<JsonArrayConst>();
  JsonArrayConst rains = daily["precipitation_probability_max"].as<JsonArrayConst>();
  JsonArrayConst codes = daily["weather_code"].as<JsonArrayConst>();
  if (codes.isNull()) codes = daily["weathercode"].as<JsonArrayConst>();   // older API name

  if (times.isNull() || maxs.isNull() || mins.isNull()) return false;

  static const char* WD[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

  int n = 0;
  for (int i = 0; i < 3 && i < (int)times.size(); i++) {
    const char* date = times[i].as<const char*>();
    if (!date || !date[0]) continue;
    if (maxs[i].isNull() || mins[i].isNull()) continue;

    ForecastDay& d = out.day[n];
    if (i == 0)      copyStr(d.label, sizeof(d.label), "Today");
    else if (i == 1) copyStr(d.label, sizeof(d.label), "Tomorrow");
    else {
      int wd = weekdayFromIsoDate(date);
      copyStr(d.label, sizeof(d.label), (wd >= 0 && wd < 7) ? WD[wd] : "Day 3");
    }

    int code = (!codes.isNull() && i < (int)codes.size() && !codes[i].isNull())
                   ? codes[i].as<int>() : -1;
    copyStr(d.cond, sizeof(d.cond), wmoLabel(code));

    d.maxC = (int8_t)lroundf((float)maxs[i].as<double>());
    d.minC = (int8_t)lroundf((float)mins[i].as<double>());

    if (!rains.isNull() && i < (int)rains.size() && !rains[i].isNull()) {
      int r = rains[i].as<int>();
      d.rainPct = (int8_t)(r < 0 ? -1 : (r > 100 ? 100 : r));
    } else {
      d.rainPct = -1;
    }
    n++;
  }

  if (n == 0) return false;
  out.valid = true;
  out.stale = false;
  return true;
}
