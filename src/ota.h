#pragma once
#include <stdint.h>

// Wireless firmware update (ArduinoOTA, pushed by `espota`).
//
// The build already uses the two-slot partition table (ota_8mb.csv: otadata +
// app0/app1), so an OTA only has to write the *other* app slot. LittleFS is
// never touched — which is why an OTA needs no re-authorisation, while
// `uploadfs` erases the device's rotated refresh token and does (see AGENTS.md).
// Nothing outside the app partition can be updated over the air: the partition
// table and the filesystem still need USB.
//
// Threading: every entry point here runs on the Arduino loop task. The receive
// loop inside ArduinoOTA::handle() writes flash and blocks for the whole image,
// so the display can only be repainted from the progress callback and pending
// state can only be flushed from the stop callback. Never call this from the
// fetch task (see fetch_task.h) — flash writes belong to loop().

// Start the OTA server. Call once, after WiFi is associated and mDNS is up.
// `password` is required: an empty or short one disables the endpoint entirely
// (logged at boot) rather than exposing an unauthenticated firmware-write
// service on the LAN.
void otaBegin(const char* password);

// Pump the OTA server. Call from loop() every iteration, as early as possible.
void otaUpdate();

// True from the moment a transfer is accepted until it ends. On success the
// device reboots immediately, so this only ever drops back to false on failure
// — which is exactly when loop() has to start polling again.
bool otaActive();

// Called with the percentage as the image lands. Painted from inside the
// blocking receive loop, so it is the only thing that can update the panel
// while an OTA is in flight — keep it quick.
void otaSetProgressCallback(void (*cb)(uint32_t pct));

// Called once when a transfer ends. `ok` true = the image is written and the
// device reboots right after: flush anything that must survive the reboot (a
// rotated refresh token still sitting in the fetch task's hand-off would be
// lost, and reusing the old one revokes the whole token family). `ok` false =
// nothing was written and the device stays up. Repaint in both cases: the
// progress screen is on the panel and loop() could not see it change.
void otaSetStopCallback(void (*cb)(bool ok));
