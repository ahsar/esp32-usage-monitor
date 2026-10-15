#include "weather_client.h"
#include "http_client.h"
#include "parse/weather_parse.h"
#include "util/loc_util.h"
#include <Arduino.h>
#include <string.h>

// Location. main.cpp pushes config.json's values here (weatherSetLocation())
// right after loadConfig(); until then these are the historical hard-coded
// Beijing values, so a config without a location block behaves as before.
static char  s_place[48]      = "Beijing";
static float s_lat            = 39.9042f;
static float s_lon            = 116.4074f;
static int   s_tzOffsetMin    = 480;          // UTC+8
static char  s_waqiStation[24] = "@3303";     // Beijing US Embassy, aqicn.org

void weatherSetLocation(const char* place, float lat, float lon, int tzOffsetMin,
                        const char* waqiStation) {
  if (place && place[0]) strlcpy(s_place, place, sizeof(s_place));
  if (waqiStation && waqiStation[0])
    strlcpy(s_waqiStation, waqiStation, sizeof(s_waqiStation));
  s_lat         = lat;
  s_lon         = lon;
  s_tzOffsetMin = tzOffsetMin;
}

static const char* WTTR_HOST = "wttr.in";
static const char* WTTR_UA   = "curl/8.4.0";

static const char* MOON_HOST = "api.met.no";
static const char* MOON_UA   = "esp32-usage-monitor/1.0";

static const char* WAQI_HOST = "api.waqi.info";

// These endpoints are small (one-line ~120 B, moon ~580 B), so an 8 s socket
// timeout is ample and keeps the worst-case blocking of the main loop bounded.
static const uint32_t NET_TIMEOUT_MS = 8000;
static const size_t   NET_MAX_BODY   = 4096;
// wttr.in and MET Norway get three attempts; waqi (apiFetchAir) gets one. The
// two small ones fail in the cheap, transient way — an empty/error body under
// load, a 503 burst — and a retry costs ~3 s, while missing them costs a whole
// poll result. The bound still holds: worst case per attempt is DNS 8 s +
// connect/handshake 40 s + read 8 s, so three attempts is ~170 s, the same order
// as the usage poll that refreshes its token twice, which is what
// JOB_DEADLINE_MS (240 s) is already sized for. waqi relies on the scheduler
// instead: a failure re-arms its timer with a cooldown (see pollerResult), and
// retrying here would only keep the single job slot busy longer.
static const int      NET_ATTEMPTS_SMALL = 3;

// ---------------------------------------------------------------------------
// weather (wttr.in one-line format)
// ---------------------------------------------------------------------------

bool apiFetchWeather(WeatherData& out) {
  bool hadData = out.valid;

  // Built per call (a few µs) rather than cached, so a location change is picked
  // up without another module needing to invalidate anything.
  char path[192];
  if (!wttrOneLinePath(s_place, path, sizeof(path))) {
    Serial.println("weather: bad place in config");
    return false;
  }

  for (int attempt = 0; attempt < NET_ATTEMPTS_SMALL; attempt++) {
    if (attempt) delay(200u * attempt);

    HttpResponse res;
    if (!httpRequest(WTTR_HOST, "GET", path, nullptr, nullptr, res, true,
                     NET_MAX_BODY, NET_TIMEOUT_MS, WTTR_UA)) {
      Serial.println("weather: request failed");
      continue;
    }
    if (res.status != 200) {
      Serial.printf("weather: HTTP %d\n", res.status);
      continue;
    }

    res.body.trim();
    if (!weatherParseLine(res.body.c_str(), out)) {
      Serial.println("weather: parse failed");
      continue;
    }

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

  char path[160];
  if (!moonApiPath(s_lat, s_lon, s_tzOffsetMin, path, sizeof(path))) {
    Serial.println("moon: bad location in config");
    return false;
  }

  for (int attempt = 0; attempt < NET_ATTEMPTS_SMALL; attempt++) {
    if (attempt) delay(200u * attempt);

    HttpResponse res;
    if (!httpRequest(MOON_HOST, "GET", path, nullptr, nullptr, res, true,
                     NET_MAX_BODY, NET_TIMEOUT_MS, MOON_UA)) {
      Serial.println("moon: request failed");
      continue;
    }
    if (res.status != 200) {
      Serial.printf("moon: HTTP %d\n", res.status);
      continue;
    }

    if (!moonParseJson(res.body.c_str(), out)) {
      Serial.println("moon: parse failed");
      continue;
    }

    Serial.printf("moon: rise=%d set=%d %s illum=%d%%\n",
                  out.moonriseMin, out.moonsetMin, out.phaseName, out.illumPct);
    return true;
  }

  if (hadData) out.stale = true;
  return false;
}

// ---------------------------------------------------------------------------
// air quality (waqi)
// ---------------------------------------------------------------------------

bool apiFetchAir(AirData& out, const char* token) {
  bool hadData = out.valid;
  if (!token || !token[0]) return false;

  String path = "/feed/";
  path += s_waqiStation;
  path += "/?token=";
  path += token;

  HttpResponse res;
  if (!httpRequest(WAQI_HOST, "GET", path.c_str(), nullptr, nullptr, res, true,
                   NET_MAX_BODY, NET_TIMEOUT_MS)) {
    Serial.println("air: request failed");
    if (hadData) out.stale = true;
    return false;
  }
  if (res.status != 200) {
    Serial.printf("air fetch failed: HTTP %d\n", res.status);
    if (hadData) out.stale = true;
    return false;
  }

  if (!airParseJson(res.body.c_str(), out)) {
    Serial.println("air parse failed");
    if (hadData) out.stale = true;
    return false;
  }

  Serial.printf("air: aqi=%d pm25=%d pm10=%d at=%s\n",
                out.aqi, out.pm25, out.pm10, out.observedAt);
  return true;
}
