#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

struct AppConfig {
  char workspace_id[128];
  char refresh_token[512];
  char org_id[128];
  char waqi_token[64];
};

inline bool loadConfig(AppConfig& cfg) {
  if (!LittleFS.begin(false)) {
    if (!LittleFS.format()) return false;
    if (!LittleFS.begin(false)) return false;
  }
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
  return true;
}
