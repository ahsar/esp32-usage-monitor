#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <string.h>
#include "storage.h"
#include "app_defaults.h"

struct AppConfig {
  char workspace_id[128];
  char refresh_token[512];
  char org_id[128];
  char waqi_token[64];
  // Shared secret for the HTTP status endpoint (HMAC-SHA256). Empty = no auth.
  char status_key[80];
  // Password for the ArduinoOTA endpoint (see ota.h). Required: shorter than 12
  // characters disables the endpoint (fail closed), it is never "no auth".
  char ota_password[64];

  // Location. `place` is both the wttr.in query and the name the status bar
  // shows; lat/lon are what the MET Norway moon API computes rise/set for;
  // `tz_offset_min` is minutes east of UTC (480 = UTC+8) and drives both
  // configTime() and the wall time the RTC is written in. `waqi_station` is the
  // aqicn.org station id the air card reads, and `air_label` is its subtitle
  // (empty = use `place`).
  char  place[48];
  float lat;
  float lon;
  int   tz_offset_min;
  char  waqi_station[24];
  char  air_label[32];

  // Network poll periods (seconds). `*_idle` is used while the device has been
  // untouched for a while, to cut radio wake-ups when nobody is looking.
  uint16_t poll_usage_s;        // active usage poll
  uint16_t poll_usage_idle_s;   // usage poll when idle
  uint16_t poll_weather_s;
  uint16_t poll_air_s;
  uint16_t poll_moon_s;

  // Feature toggles.
  bool chime;          // play a chime when the agent finishes / needs you
  // Serve the device channel over Bluetooth LE as well as HTTP (ble_status.h):
  // the fallback for when the PC cannot reach the device over the LAN. Costs
  // advertising plus the stack's RAM, so it is a toggle rather than a given.
  bool ble_status;
};

// `ble_status` builds in on by default, but can be turned off for a whole build
// without touching the filesystem — which is the point: the alternative,
// editing data/config.json, needs an `uploadfs`, and that erases the device's
// rotated OAuth tokens (AGENTS.md). It is also the only way to A/B the BLE
// stack's cost, e.g. to see what it does to the internal heap the pollers and
// mbedTLS share:
//
//   PLATFORMIO_BUILD_FLAGS='-DDEFAULT_BLE_STATUS=0' \
//     pio run -e esp32-usage-monitor -t clean -t upload
#ifndef DEFAULT_BLE_STATUS
#define DEFAULT_BLE_STATUS 1
#endif

// Fill in every field with a sane default. Called before parsing so a missing
// or malformed config.json still leaves a usable configuration.
inline void configDefaults(AppConfig& cfg) {  memset(&cfg, 0, sizeof(cfg));
  // The location defaults are the ones this firmware was written around, so a
  // config.json without the location block behaves exactly as it always has.
  strlcpy(cfg.place,        DEFAULT_PLACE,         sizeof(cfg.place));
  cfg.lat           = DEFAULT_LAT;
  cfg.lon           = DEFAULT_LON;
  cfg.tz_offset_min = DEFAULT_TZ_OFFSET_MIN;
  strlcpy(cfg.waqi_station, DEFAULT_WAQI_STATION, sizeof(cfg.waqi_station));
  cfg.poll_usage_s      = DEFAULT_POLL_USAGE_S;
  cfg.poll_usage_idle_s = DEFAULT_POLL_USAGE_IDLE_S;
  cfg.poll_weather_s    = DEFAULT_POLL_WEATHER_S;
  cfg.poll_air_s        = DEFAULT_POLL_AIR_S;
  cfg.poll_moon_s       = DEFAULT_POLL_MOON_S;
  cfg.chime             = true;
  cfg.ble_status        = (bool)DEFAULT_BLE_STATUS;
}

// Copy an optional string field, keeping whatever configDefaults() wrote when
// the key is absent or empty (so `"place": ""` cannot blank the location).
static inline void cfgStr(const JsonVariant v, char* dst, size_t cap) {
  const char* s = v | "";
  if (s && s[0]) strlcpy(dst, s, cap);
}

inline bool loadConfig(AppConfig& cfg) {
  configDefaults(cfg);
  if (!storageMount()) return false;
  File f = LittleFS.open("/config.json", "r");
  if (!f) return false;
  JsonDocument doc;
  if (deserializeJson(doc, f) != DeserializationError::Ok) {
    f.close(); return false;
  }
  f.close();
  strlcpy(cfg.workspace_id,  doc["workspace_id"]  | "", sizeof(cfg.workspace_id));
  strlcpy(cfg.refresh_token, doc["refresh_token"] | "", sizeof(cfg.refresh_token));
  strlcpy(cfg.org_id,        doc["org_id"]        | "", sizeof(cfg.org_id));
  strlcpy(cfg.waqi_token,    doc["waqi_token"]    | "", sizeof(cfg.waqi_token));
  strlcpy(cfg.status_key,    doc["status_key"]    | "", sizeof(cfg.status_key));
  strlcpy(cfg.ota_password,  doc["ota_password"]  | "", sizeof(cfg.ota_password));
  cfgStr(doc["place"],        cfg.place,        sizeof(cfg.place));
  cfgStr(doc["waqi_station"], cfg.waqi_station, sizeof(cfg.waqi_station));
  cfgStr(doc["air_label"],    cfg.air_label,    sizeof(cfg.air_label));
  cfg.lat           = doc["lat"]           | cfg.lat;
  cfg.lon           = doc["lon"]           | cfg.lon;
  cfg.tz_offset_min = doc["tz_offset_min"] | cfg.tz_offset_min;
  cfg.poll_usage_s      = doc["poll_usage_s"]      | cfg.poll_usage_s;
  cfg.poll_usage_idle_s = doc["poll_usage_idle_s"] | cfg.poll_usage_idle_s;
  cfg.poll_weather_s    = doc["poll_weather_s"]    | cfg.poll_weather_s;
  cfg.poll_air_s        = doc["poll_air_s"]        | cfg.poll_air_s;
  cfg.poll_moon_s       = doc["poll_moon_s"]       | cfg.poll_moon_s;
  cfg.chime             = doc["chime"]             | cfg.chime;
  cfg.ble_status        = doc["ble_status"]        | cfg.ble_status;
  return true;
}
