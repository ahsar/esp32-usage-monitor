#pragma once
#include <stdint.h>

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

bool apiFetchWeather(WeatherData& out);
bool apiFetchMoon(MoonData& out);
bool apiFetchAir(AirData& out, const char* token);
