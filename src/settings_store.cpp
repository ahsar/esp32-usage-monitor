#include "settings_store.h"
#include "storage.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <string.h>

// Everything that is overridden, serialized, is well under this; the cap is only
// here so a runaway settings set fails loudly instead of writing a truncated file
// (which settingsFromJson() would then reject at the next boot).
static const size_t SETTINGS_JSON_CAP = 512;
static const char*  SETTINGS_PATH     = "/settings.json";
static const char*  SETTINGS_TMP      = "/settings.json.tmp";

bool settingsLoad(DeviceSettings& out) {
  settingsClear(out);
  if (!storageMount()) return false;
  File f = LittleFS.open(SETTINGS_PATH, "r");
  if (!f) return false;

  char buf[SETTINGS_JSON_CAP];
  size_t n = f.readBytes(buf, sizeof(buf) - 1);
  f.close();
  buf[n] = '\0';
  if (n == 0) return false;

  uint16_t changed = SET_NONE;
  DeviceSettings loaded;
  if (!settingsFromJson(buf, loaded, changed)) {
    Serial.println("settings: /settings.json is invalid; using config.json");
    return false;
  }
  out = loaded;
  Serial.printf("settings: overrides 0x%02x from %s\n",
                (unsigned)changed, SETTINGS_PATH);
  return true;
}

bool settingsSave(const DeviceSettings& s) {
  if (!storageMount()) return false;

  char buf[SETTINGS_JSON_CAP];
  if (!settingsToJson(s, buf, sizeof(buf))) {
    Serial.println("settings: serialization failed; not saving");
    return false;
  }
  size_t len = strlen(buf);

  // Same pattern as tokenStoreSave(): temp file, verified bytes, rename over
  // the live one. A settings file lost to a short write just reverts every
  // override at the next boot — but the failure is invisible (the device then
  // claims values it no longer has), so it is worth the two extra lines.
  File f = LittleFS.open(SETTINGS_TMP, "w");
  if (!f) {
    Serial.println("settings: cannot open /settings.json.tmp for writing");
    return false;
  }
  size_t n = f.write((const uint8_t*)buf, len);
  f.close();
  if (n != len) {
    Serial.println("settings: short write; keeping the old overrides");
    LittleFS.remove(SETTINGS_TMP);
    return false;
  }
  if (!LittleFS.rename(SETTINGS_TMP, SETTINGS_PATH)) {
    LittleFS.remove(SETTINGS_PATH);
    if (!LittleFS.rename(SETTINGS_TMP, SETTINGS_PATH)) {
      LittleFS.remove(SETTINGS_TMP);
      Serial.println("settings: rename over /settings.json failed");
      return false;
    }
  }
  return true;
}

void settingsOverlay(const DeviceSettings& s, AppConfig& cfg) {
  // Brightness has no AppConfig field: it is a display knob owned by main.cpp,
  // which applies it live (see applyDeviceSettings()) rather than at boot only.
  if (s.set & SET_CHIME)     cfg.chime = s.chime != 0;
  if (s.set & SET_PLACE)     strlcpy(cfg.place, s.place, sizeof(cfg.place));
  if (s.set & SET_COORDS)    { cfg.lat = s.lat; cfg.lon = s.lon; }
  if (s.set & SET_TZ)        cfg.tz_offset_min = s.tz_offset_min;
  if (s.set & SET_STATION)   strlcpy(cfg.waqi_station, s.waqi_station,
                                     sizeof(cfg.waqi_station));
  if (s.set & SET_AIR_LABEL) strlcpy(cfg.air_label, s.air_label, sizeof(cfg.air_label));
}
