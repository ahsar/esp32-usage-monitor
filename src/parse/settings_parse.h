#pragma once
// Device settings: the runtime overrides of config.json that the HTTP settings
// channel writes (see status_server.h). Header-only and ArduinoJson-based like
// the other payload parsers, so the whole "what did the request mean, is it in
// range, what do we store" decision is host-tested (test/test_util) and the
// firmware side only has to apply the result.
//
// A set of overrides is *partial* on purpose: a field with its bit clear follows
// config.json, so clearing an override puts the configured value back. That is
// also why every value carries a bit instead of a sentinel.
#include <ArduinoJson.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/url_util.h"

enum SettingsField : uint16_t {
  SET_NONE       = 0,
  SET_CHIME      = 1u << 0,
  SET_BRIGHTNESS = 1u << 1,
  SET_PLACE      = 1u << 2,
  SET_COORDS     = 1u << 3,   // lat and lon move together
  SET_TZ         = 1u << 4,
  SET_STATION    = 1u << 5,
  SET_AIR_LABEL  = 1u << 6,
  SET_VOLUME     = 1u << 7,
};

// Field limits. Brightness is 1..4: 0 is the idle-dim level (BRIGHT_LUT[0]), so
// it must stay impossible to set the awake level there. The coordinate and
// offset ranges are the real-world ones; anything outside is a typo.
constexpr int SETTINGS_BRIGHTNESS_MIN = 1;
constexpr int SETTINGS_BRIGHTNESS_MAX = 4;
constexpr int SETTINGS_TZ_MIN         = -16 * 60;
constexpr int SETTINGS_TZ_MAX         =  16 * 60;
// Chime loudness: an index into chime.cpp's amplitude table. 0 is the quietest
// step (not silence — that is `chime=0`), 5 the loudest.
constexpr int SETTINGS_VOLUME_MIN     = 0;
constexpr int SETTINGS_VOLUME_MAX     = 5;

// Buffer sizes shared with AppConfig's location fields, so an override can never
// be longer than the value it replaces.
constexpr size_t SETTINGS_PLACE_CAP   = 48;
constexpr size_t SETTINGS_STATION_CAP = 24;
constexpr size_t SETTINGS_AIRLABEL_CAP = 32;

struct DeviceSettings {
  uint16_t set = SET_NONE;     // which fields below are overridden
  int   chime = 0;             // 0/1
  int   brightness = 0;        // BRIGHT_LUT index, 1..4
  int   volume = 0;            // chime loudness index, 0..5
  char  place[SETTINGS_PLACE_CAP] = {0};
  float lat = 0, lon = 0;
  int   tz_offset_min = 0;
  char  waqi_station[SETTINGS_STATION_CAP] = {0};
  char  air_label[SETTINGS_AIRLABEL_CAP] = {0};
};

inline void settingsClear(DeviceSettings& s) { s = DeviceSettings(); }

// ---- range checks ---------------------------------------------------------

inline bool settingsBrightnessOk(int v) {
  return v >= SETTINGS_BRIGHTNESS_MIN && v <= SETTINGS_BRIGHTNESS_MAX;
}
inline bool settingsChimeOk(int v) { return v == 0 || v == 1; }
inline bool settingsVolumeOk(int v) {
  return v >= SETTINGS_VOLUME_MIN && v <= SETTINGS_VOLUME_MAX;
}
inline bool settingsTzOk(int v) { return v >= SETTINGS_TZ_MIN && v <= SETTINGS_TZ_MAX; }
inline bool settingsLatOk(float v) { return v >= -90.0f && v <= 90.0f; }
inline bool settingsLonOk(float v) { return v >= -180.0f && v <= 180.0f; }

// "1"/"true"/"on" and "0"/"false"/"off" -> 0/1; anything else -> -1.
inline int settingsParseBool(const char* v) {
  if (!v) return -1;
  if (!strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "on")) return 1;
  if (!strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "off")) return 0;
  return -1;
}

// Whole-string number parse: "4x" or "abc" must be rejected, not read as 4 or 0.
inline bool settingsParseInt(const char* v, long& out) {
  if (!v || !v[0]) return false;
  char* end = nullptr;
  long n = strtol(v, &end, 10);
  if (!end || *end) return false;
  out = n;
  return true;
}

inline bool settingsParseFloat(const char* v, float& out) {
  if (!v || !v[0]) return false;
  char* end = nullptr;
  float f = strtof(v, &end);
  if (!end || *end) return false;
  out = f;
  return true;
}

// Copy into `dst`, rejecting anything that does not fit (instead of truncating:
// a silently shortened place name would just look like wttr.in misbehaving).
// Spelled out rather than using weather_parse.h's copyStr(), which truncates, or
// strlcpy(), which is not portable to the host build (see weather_parse.h).
inline bool settingsCopyStr(char* dst, size_t cap, const char* v) {
  if (!dst || cap == 0) return false;
  size_t n = v ? strlen(v) : 0;
  if (n >= cap) return false;
  memcpy(dst, v ? v : "", n);
  dst[n] = '\0';
  return true;
}

// ---- applying a query -----------------------------------------------------

// Apply a raw query string (no leading '?', not yet decoded), e.g.
// "chime=0&place=Shanghai&brightness=4", on top of `s`.
//
// - an empty value clears the override, so the field follows config.json again;
// - lat and lon must be given together (one without the other is an error);
// - `changed` receives the fields this query touched, which is what the caller
//   uses to decide what to push to the live device;
// - validation is all-or-nothing: a typo (unknown key, out of range, too long,
//   bad number) returns false with a message and leaves `s` untouched, so a
//   malformed request can never half-configure the device.
inline bool settingsFromQuery(const char* query, DeviceSettings& s, uint16_t& changed,
                             char* err, size_t errCap) {
  DeviceSettings next = s;
  uint16_t touched = SET_NONE;
  bool haveLat = false, haveLon = false;
  float lat = 0, lon = 0;

  auto fail = [&](const char* msg) {
    if (err && errCap) snprintf(err, errCap, "%s", msg);
    return false;
  };

  if (!query || !query[0]) return fail("empty settings query");

  for (const char* p = query; p && *p; ) {
    const char* amp = strchr(p, '&');
    size_t pairLen = amp ? (size_t)(amp - p) : strlen(p);

    // Sized to the whole query the transport accepts (status_server caps it at
    // 256), so "too long" is reported as such rather than as a malformed pair.
    char pair[256], key[40], rawVal[160];
    if (pairLen == 0) return fail("malformed settings query");
    if (pairLen >= sizeof(pair)) return fail("settings query too long");
    memcpy(pair, p, pairLen);
    pair[pairLen] = '\0';
    p = amp ? amp + 1 : p + pairLen;

    char* eq = strchr(pair, '=');
    if (!eq) return fail("expected key=value");
    *eq = '\0';
    if (!urlDecode(pair, key, sizeof(key))) return fail("settings key too long");
    if (!urlDecode(eq + 1, rawVal, sizeof(rawVal))) return fail("settings value too long");
    const char* v = rawVal;

    if (!strcmp(key, "chime")) {
      if (!v[0])      { next.set &= (uint16_t)~SET_CHIME; next.chime = 0; }
      else {
        int b = settingsParseBool(v);
        if (b < 0) return fail("chime must be 0/1 (or true/false)");
        next.chime = b; next.set |= SET_CHIME;
      }
      touched |= SET_CHIME;
    } else if (!strcmp(key, "brightness")) {
      if (!v[0]) { next.set &= (uint16_t)~SET_BRIGHTNESS; next.brightness = 0; }
      else {
        long n = 0;
        if (!settingsParseInt(v, n) || !settingsBrightnessOk((int)n))
          return fail("brightness must be 1..4 (0 is the dim level)");
        next.brightness = (int)n; next.set |= SET_BRIGHTNESS;
      }
      touched |= SET_BRIGHTNESS;
    } else if (!strcmp(key, "volume")) {
      if (!v[0]) { next.set &= (uint16_t)~SET_VOLUME; next.volume = 0; }
      else {
        long n = 0;
        if (!settingsParseInt(v, n) || !settingsVolumeOk((int)n))
          return fail("volume must be 0..5 (chime=0 turns chimes off)");
        next.volume = (int)n; next.set |= SET_VOLUME;
      }
      touched |= SET_VOLUME;
    } else if (!strcmp(key, "place")) {
      if (!v[0]) { next.set &= (uint16_t)~SET_PLACE; next.place[0] = '\0'; }
      else {
        if (!settingsCopyStr(next.place, sizeof(next.place), v))
          return fail("place too long (max 47 characters)");
        next.set |= SET_PLACE;
      }
      touched |= SET_PLACE;
    } else if (!strcmp(key, "lat") || !strcmp(key, "lon")) {
      bool isLat = (key[0] == 'l' && key[1] == 'a');
      float f = 0;
      if (!settingsParseFloat(v, f) || (isLat ? !settingsLatOk(f) : !settingsLonOk(f)))
        return fail(isLat ? "lat must be -90..90" : "lon must be -180..180");
      if (isLat) { lat = f; haveLat = true; }
      else       { lon = f; haveLon = true; }
      touched |= SET_COORDS;
    } else if (!strcmp(key, "tz_offset_min")) {
      if (!v[0]) { next.set &= (uint16_t)~SET_TZ; next.tz_offset_min = 0; }
      else {
        long n = 0;
        if (!settingsParseInt(v, n) || !settingsTzOk((int)n))
          return fail("tz_offset_min must be -960..960");
        next.tz_offset_min = (int)n; next.set |= SET_TZ;
      }
      touched |= SET_TZ;
    } else if (!strcmp(key, "waqi_station")) {
      if (!v[0]) { next.set &= (uint16_t)~SET_STATION; next.waqi_station[0] = '\0'; }
      else {
        if (!settingsCopyStr(next.waqi_station, sizeof(next.waqi_station), v))
          return fail("waqi_station too long (max 23 characters)");
        next.set |= SET_STATION;
      }
      touched |= SET_STATION;
    } else if (!strcmp(key, "air_label")) {
      if (!v[0]) { next.set &= (uint16_t)~SET_AIR_LABEL; next.air_label[0] = '\0'; }
      else {
        if (!settingsCopyStr(next.air_label, sizeof(next.air_label), v))
          return fail("air_label too long (max 31 characters)");
        next.set |= SET_AIR_LABEL;
      }
      touched |= SET_AIR_LABEL;
    } else {
      return fail("unknown setting");
    }
  }

  // Coordinates are one place: half of a location would silently move the moon
  // calculations and not the weather query.
  if (haveLat || haveLon) {
    if (!(haveLat && haveLon)) return fail("lat and lon must be set together");
    next.lat = lat; next.lon = lon; next.set |= SET_COORDS;
  }

  s = next;
  changed = touched;
  return true;
}

// ---- persistence (/settings.json) ----------------------------------------

// Serialize the overrides for /settings.json. Only the set fields are written, so
// the file stays a readable record of what was changed over HTTP. Returns false
// when they do not fit in `cap` (the caller keeps the previous file rather than
// writing a truncated one).
inline bool settingsToJson(const DeviceSettings& s, char* out, size_t cap) {
  JsonDocument doc;
  JsonObject o = doc.to<JsonObject>();   // without this an empty set serializes as "null"
  if (s.set & SET_CHIME)      o["chime"]          = s.chime;
  if (s.set & SET_BRIGHTNESS) o["brightness"]     = s.brightness;
  if (s.set & SET_VOLUME)     o["volume"]         = s.volume;
  if (s.set & SET_PLACE)      o["place"]          = s.place;
  if (s.set & SET_COORDS)     { o["lat"] = s.lat; o["lon"] = s.lon; }
  if (s.set & SET_TZ)         o["tz_offset_min"]  = s.tz_offset_min;
  if (s.set & SET_STATION)    o["waqi_station"]   = s.waqi_station;
  if (s.set & SET_AIR_LABEL)  o["air_label"]      = s.air_label;

  if (measureJson(doc) + 1 > cap) return false;
  return serializeJson(doc, out, cap) > 0;
}

// Load overrides from a /settings.json document. Unknown keys are ignored (so a
// newer writer cannot brick an older device), but a bad value or a non-object
// document fails the whole load — the caller then falls back to config.json
// rather than half-applying a corrupt file.
inline bool settingsFromJson(const char* json, DeviceSettings& s, uint16_t& changed) {
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) return false;
  if (!doc.is<JsonObject>()) return false;

  DeviceSettings next = s;
  uint16_t touched = SET_NONE;
  bool haveLat = false, haveLon = false;

  for (JsonPair kv : doc.as<JsonObject>()) {
    const char* k = kv.key().c_str();
    JsonVariant v = kv.value();

    if (!strcmp(k, "chime")) {
      int b = -1;
      if (v.is<bool>())          b = v.as<bool>() ? 1 : 0;
      else if (v.is<int>())      b = (v.as<int>() != 0) ? 1 : 0;
      else if (v.is<const char*>()) b = settingsParseBool(v.as<const char*>());
      if (b < 0) return false;
      next.chime = b; next.set |= SET_CHIME; touched |= SET_CHIME;
    } else if (!strcmp(k, "brightness")) {
      int n = v.as<int>();
      if (!v.is<int>() || !settingsBrightnessOk(n)) return false;
      next.brightness = n; next.set |= SET_BRIGHTNESS; touched |= SET_BRIGHTNESS;
    } else if (!strcmp(k, "volume")) {
      int n = v.as<int>();
      if (!v.is<int>() || !settingsVolumeOk(n)) return false;
      next.volume = n; next.set |= SET_VOLUME; touched |= SET_VOLUME;
    } else if (!strcmp(k, "place")) {
      const char* p = v.as<const char*>();
      if (!p || !p[0] || !settingsCopyStr(next.place, sizeof(next.place), p)) return false;
      next.set |= SET_PLACE; touched |= SET_PLACE;
    } else if (!strcmp(k, "lat") || !strcmp(k, "lon")) {
      float f = v.as<float>();
      bool isLat = (k[1] == 'a');
      if (isLat ? !settingsLatOk(f) : !settingsLonOk(f)) return false;
      if (isLat) { next.lat = f; haveLat = true; }
      else       { next.lon = f; haveLon = true; }
      next.set |= SET_COORDS; touched |= SET_COORDS;
    } else if (!strcmp(k, "tz_offset_min")) {
      int n = v.as<int>();
      if (!settingsTzOk(n)) return false;
      next.tz_offset_min = n; next.set |= SET_TZ; touched |= SET_TZ;
    } else if (!strcmp(k, "waqi_station")) {
      const char* p = v.as<const char*>();
      if (!p || !p[0] || !settingsCopyStr(next.waqi_station, sizeof(next.waqi_station), p))
        return false;
      next.set |= SET_STATION; touched |= SET_STATION;
    } else if (!strcmp(k, "air_label")) {
      const char* p = v.as<const char*>();
      if (!p || !p[0] || !settingsCopyStr(next.air_label, sizeof(next.air_label), p))
        return false;
      next.set |= SET_AIR_LABEL; touched |= SET_AIR_LABEL;
    }
  }

  // Coordinates must have arrived as a pair, for the same reason as in the query
  // path.
  if ((touched & SET_COORDS) && !(haveLat && haveLon)) return false;

  s = next;
  changed = touched;
  return true;
}

// ---- describing them ------------------------------------------------------

inline size_t settingsAppend(char* out, size_t cap, size_t n, const char* text) {
  for (const char* p = text; p && *p; p++) {
    if (n + 1 >= cap) break;
    out[n++] = *p;
  }
  if (cap) out[n] = '\0';
  return n;
}

// Human-readable summary of the overrides — the body of a settings request, so a
// plain `curl` shows what the device is actually running with. Only the set
// fields are listed; the rest follow config.json.
inline void settingsDescribe(const DeviceSettings& s, char* out, size_t cap) {
  if (!cap) return;
  out[0] = '\0';
  size_t n = 0;

  if (!s.set) {
    settingsAppend(out, cap, n, "no overrides (everything follows config.json)\n");
    return;
  }

  settingsAppend(out, cap, n, "overrides (unset fields follow config.json)\n");
  char line[96];
  if (s.set & SET_CHIME) {
    snprintf(line, sizeof(line), "  chime          = %s\n", s.chime ? "on" : "off");
    n = settingsAppend(out, cap, n, line);
  }
  if (s.set & SET_BRIGHTNESS) {
    snprintf(line, sizeof(line), "  brightness     = %d\n", s.brightness);
    n = settingsAppend(out, cap, n, line);
  }
  if (s.set & SET_VOLUME) {
    snprintf(line, sizeof(line), "  volume         = %d\n", s.volume);
    n = settingsAppend(out, cap, n, line);
  }
  if (s.set & SET_PLACE) {
    snprintf(line, sizeof(line), "  place          = %s\n", s.place);
    n = settingsAppend(out, cap, n, line);
  }
  if (s.set & SET_COORDS) {
    snprintf(line, sizeof(line), "  lat,lon        = %.4f,%.4f\n",
             (double)s.lat, (double)s.lon);
    n = settingsAppend(out, cap, n, line);
  }
  if (s.set & SET_TZ) {
    snprintf(line, sizeof(line), "  tz_offset_min  = %d\n", s.tz_offset_min);
    n = settingsAppend(out, cap, n, line);
  }
  if (s.set & SET_STATION) {
    snprintf(line, sizeof(line), "  waqi_station   = %s\n", s.waqi_station);
    n = settingsAppend(out, cap, n, line);
  }
  if (s.set & SET_AIR_LABEL) {
    snprintf(line, sizeof(line), "  air_label      = %s\n", s.air_label);
    settingsAppend(out, cap, n, line);
  }
}
