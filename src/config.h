#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <string.h>
#include "storage.h"

struct AppConfig {
  char workspace_id[128];
  char refresh_token[512];
  char org_id[128];
  char waqi_token[64];
  // Shared secret for the HTTP status endpoint (HMAC-SHA256). Empty = no auth.
  char status_key[80];

  // Network poll periods (seconds). `*_idle` is used while the device has been
  // untouched for a while, to cut radio wake-ups when nobody is looking.
  uint16_t poll_usage_s;        // active usage poll
  uint16_t poll_usage_idle_s;   // usage poll when idle
  uint16_t poll_weather_s;
  uint16_t poll_air_s;
  uint16_t poll_moon_s;

  // Feature toggles.
  bool chime;          // play a chime when the agent finishes / needs you
  bool imu_gestures;   // shake / double-tap / pickup / face-down / rotate
};

// Fill in every field with a sane default. Called before parsing so a missing
// or malformed config.json still leaves a usable configuration.
inline void configDefaults(AppConfig& cfg) {
  memset(&cfg, 0, sizeof(cfg));
  cfg.poll_usage_s      = 60;
  cfg.poll_usage_idle_s = 180;
  cfg.poll_weather_s    = 1800;
  cfg.poll_air_s        = 3600;
  cfg.poll_moon_s       = 43200;
  cfg.chime             = true;
  cfg.imu_gestures      = true;
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
  cfg.poll_usage_s      = doc["poll_usage_s"]      | cfg.poll_usage_s;
  cfg.poll_usage_idle_s = doc["poll_usage_idle_s"] | cfg.poll_usage_idle_s;
  cfg.poll_weather_s    = doc["poll_weather_s"]    | cfg.poll_weather_s;
  cfg.poll_air_s        = doc["poll_air_s"]        | cfg.poll_air_s;
  cfg.poll_moon_s       = doc["poll_moon_s"]       | cfg.poll_moon_s;
  cfg.chime             = doc["chime"]             | cfg.chime;
  cfg.imu_gestures      = doc["imu_gestures"]      | cfg.imu_gestures;
  return true;
}
