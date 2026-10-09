#include "weather_client.h"
#include "http_client.h"
#include "util/time_util.h"
#include "util/value_util.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

// Beijing — matches the user's fixed location.
static const char* WTTR_HOST = "wttr.in";
// One-line format: location | condition | temp | feels | humidity | wind |
//                  precip | sunrise | sunset
static const char* WTTR_PATH = "/Beijing?m&format=%l|%C|%t|%f|%h|%w|%p|%S|%s";
static const char* WTTR_UA   = "curl/8.4.0";

static const char* MOON_HOST = "api.met.no";
static const char* MOON_PATH =
    "/weatherapi/sunrise/3.0/moon?lat=39.9042&lon=116.4074&offset=%2B08:00";
static const char* MOON_UA   = "esp32-usage-monitor/1.0";

static const char* WAQI_HOST = "api.waqi.info";
// Beijing US Embassy monitoring station on aqicn.org.
static const char* WAQI_ID   = "@3303";

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

static int parseSignedInt(const char* s) {
  if (!s) return 0;
  while (*s && (*s == '+' || *s == ' ')) s++;
  return atoi(s);
}

// wttr.in wind arrow (UTF-8 e2 86 9x) -> 16-point compass string.
static void windDirFromArrow(const char* w, char* out, size_t outSz) {
  static const char* DIRS[8] = { "S", "SW", "W", "NW", "N", "NE", "E", "SE" };
  const unsigned char* p = (const unsigned char*)w;
  int idx = 0;
  while (*p) {
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
  strlcpy(out, DIRS[idx], outSz);
}

// ---------------------------------------------------------------------------
// weather (wttr.in one-line format)
// ---------------------------------------------------------------------------

// Split "a|b|c" into up to n fields; returns the number found.
static int splitFields(const String& s, char fields[][48], int n) {
  int count = 0;
  int start = 0;
  for (int i = 0; i <= (int)s.length() && count < n; i++) {
    if (i == (int)s.length() || s[i] == '|') {
      String f = s.substring(start, i);
      f.trim();
      strlcpy(fields[count], f.c_str(), 48);
      count++;
      start = i + 1;
    }
  }
  return count;
}

bool apiFetchWeather(WeatherData& out) {
  bool hadData = out.valid;

  for (int attempt = 0; attempt < 2; attempt++) {
    if (attempt) delay(200);

    HttpResponse res;
    if (!httpRequest(WTTR_HOST, "GET", WTTR_PATH, nullptr, nullptr, res, true, 8192, 12000,
                     WTTR_UA)) {
      Serial.println("weather: request failed");
      continue;
    }
    if (res.status != 200) {
      Serial.printf("weather: HTTP %d\n", res.status);
      continue;
    }

    res.body.trim();
    char f[9][48];
    int n = splitFields(res.body, f, 9);
    if (n < 9) {
      Serial.printf("weather: only %d fields\n", n);
      continue;
    }

    strlcpy(out.area, f[0], sizeof(out.area));
    strlcpy(out.condition, f[1], sizeof(out.condition));
    out.tempC    = parseSignedInt(f[2]);
    out.feelsC   = parseSignedInt(f[3]);
    out.humidity = atoi(f[4]);
    windDirFromArrow(f[5], out.windDir, sizeof(out.windDir));
    out.windKmph = atoi(strpbrk(f[5], "0123456789") ? strpbrk(f[5], "0123456789") : "0");
    out.precipTenths = (int)(atof(f[6]) * 10.0f + 0.5f);
    out.sunriseMin = parseHms(f[7]);
    out.sunsetMin  = parseHms(f[8]);

    out.valid = true;
    out.stale = false;
    out.obsTime[0] = 0;
    Serial.printf("weather: %dC %s hum=%d wind=%d%s precip=%d sun=%d/%d\n",
                  out.tempC, out.condition, out.humidity, out.windKmph, out.windDir,
                  out.precipTenths, out.sunriseMin, out.sunsetMin);
    return true;
  }

  if (hadData) out.stale = true;
  return false;
}

// ---------------------------------------------------------------------------
// moon (MET Norway)
// ---------------------------------------------------------------------------

bool apiFetchMoon(MoonData& out) {
  bool hadData = out.valid;

  for (int attempt = 0; attempt < 2; attempt++) {
    if (attempt) delay(200);

    HttpResponse res;
    if (!httpRequest(MOON_HOST, "GET", MOON_PATH, nullptr, nullptr, res, true, 8192, 12000,
                     MOON_UA)) {
      Serial.println("moon: request failed");
      continue;
    }
    if (res.status != 200) {
      Serial.printf("moon: HTTP %d\n", res.status);
      continue;
    }

    JsonDocument doc;
    if (deserializeJson(doc, res.body) != DeserializationError::Ok) {
      Serial.println("moon: parse failed");
      continue;
    }

    JsonObjectConst props = doc["properties"].as<JsonObjectConst>();
    if (props.isNull()) {
      Serial.println("moon: no properties");
      continue;
    }

    out.moonriseMin = parseIsoLocalMinutes(props["moonrise"]["time"] | "");
    out.moonsetMin  = parseIsoLocalMinutes(props["moonset"]["time"] | "");

    float phase = 0.0f;
    JsonVariantConst mp = props["moonphase"];
    if (!mp.isNull()) phase = mp.as<float>();
    float p = phase / 360.0f;
    while (p < 0) p += 1.0f;
    while (p >= 1) p -= 1.0f;

    out.waxing   = (p < 0.5f);
    out.illumPct = (int)((1.0f - cosf(2.0f * (float)M_PI * p)) * 50.0f + 0.5f);
    strlcpy(out.phaseName, phaseNameFor(p), sizeof(out.phaseName));

    out.valid = true;
    out.stale = false;
    Serial.printf("moon: rise=%d set=%d phase=%.2f %s illum=%d%%\n",
                  out.moonriseMin, out.moonsetMin, p, out.phaseName, out.illumPct);
    return true;
  }

  if (hadData) out.stale = true;
  return false;
}

// ---------------------------------------------------------------------------
// air quality (waqi)
// ---------------------------------------------------------------------------

static int iaqi(JsonVariantConst iaqiObj, const char* key) {
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

bool apiFetchAir(AirData& out, const char* token) {
  bool hadData = out.valid;
  if (!token || !token[0]) return false;

  String path = "/feed/";
  path += WAQI_ID;
  path += "/?token=";
  path += token;

  HttpResponse res;
  if (!httpRequest(WAQI_HOST, "GET", path.c_str(), nullptr, nullptr, res, true, 8192, 12000)) {
    Serial.println("air: request failed");
    if (hadData) out.stale = true;
    return false;
  }
  if (res.status != 200) {
    Serial.printf("air fetch failed: HTTP %d\n", res.status);
    if (hadData) out.stale = true;
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, res.body) != DeserializationError::Ok) {
    Serial.println("air parse failed");
    if (hadData) out.stale = true;
    return false;
  }

  const char* status = doc["status"] | "";
  if (strcmp(status, "ok") != 0) {
    Serial.printf("air status: %s\n", status);
    if (hadData) out.stale = true;
    return false;
  }

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
  if (strlen(ts) >= 16) strlcpy(out.observedAt, ts + 11, 6);
  else out.observedAt[0] = 0;

  out.valid = true;
  out.stale = false;
  Serial.printf("air: aqi=%d pm25=%d pm10=%d at=%s\n", out.aqi, out.pm25, out.pm10, out.observedAt);
  return true;
}
