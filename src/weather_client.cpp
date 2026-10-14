#include "weather_client.h"
#include "http_client.h"
#include "parse/weather_parse.h"
#include <Arduino.h>
#include <string.h>

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

// These endpoints are small (one-line ~120 B, moon ~580 B), so an 8 s socket
// timeout is ample and keeps the worst-case blocking of the main loop bounded.
static const uint32_t NET_TIMEOUT_MS = 8000;
static const size_t   NET_MAX_BODY   = 4096;
// One attempt per poll: a failed job is backed off by the scheduler
// (main.cpp), and retrying here only keeps the single job slot busy longer.
static const int      NET_ATTEMPTS   = 1;

// ---------------------------------------------------------------------------
// weather (wttr.in one-line format)
// ---------------------------------------------------------------------------

bool apiFetchWeather(WeatherData& out) {
  bool hadData = out.valid;

  for (int attempt = 0; attempt < NET_ATTEMPTS; attempt++) {
    if (attempt) delay(200);

    HttpResponse res;
    if (!httpRequest(WTTR_HOST, "GET", WTTR_PATH, nullptr, nullptr, res, true,
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

  for (int attempt = 0; attempt < NET_ATTEMPTS; attempt++) {
    if (attempt) delay(200);

    HttpResponse res;
    if (!httpRequest(MOON_HOST, "GET", MOON_PATH, nullptr, nullptr, res, true,
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
  path += WAQI_ID;
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
