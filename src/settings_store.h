#pragma once
#include "config.h"
#include "parse/settings_parse.h"

// Device settings: the runtime overrides of config.json that the HTTP settings
// channel writes (status_server.h). They live in their own file, /settings.json,
// rather than being written back into config.json:
//
//   - config.json is the *packed* file (it ships inside the LittleFS image from
//     data/config.json), so rewriting it on the device would make the next
//     `uploadfs` and the firmware disagree about what is authoritative — and the
//     uploadfs token guard reasons about that exact file;
//   - an override file can be deleted to get back to the configured values
//     without touching a token.
//
// Precedence at boot: config.json (via loadConfig()) first, then
// settingsOverlay() on top, so an override always wins and clearing it reveals
// the configured value again.
//
// Threading: settingsSave() writes flash, so it runs on the loop task only
// (handleHttpStatus() is called from loop()) — same rule as the token flush, and
// never from the fetch task.

// Read /settings.json. Returns false when it is absent, unreadable or invalid;
// `out` is then left cleared (no overrides), never half-applied.
bool settingsLoad(DeviceSettings& out);

// Write the overrides out. Returns false when the serialization or the write
// failed, in which case the caller must not claim the change took effect.
bool settingsSave(const DeviceSettings& s);

// Copy each overridden field into `cfg`, leaving unset fields alone. Start from a
// config.json-only AppConfig every time, so clearing an override restores the
// configured value instead of leaving the old one behind.
void settingsOverlay(const DeviceSettings& s, AppConfig& cfg);
