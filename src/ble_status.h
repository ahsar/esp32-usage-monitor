#pragma once
#include <stdint.h>

// The device channel over Bluetooth LE — the fallback for when the PC cannot
// reach the device over the LAN. The two are the same channel (status_channel.h):
// the same state pings, the same settings requests, the same HMAC. Only the
// framing differs, and both ends go through statusChannelHandle()
// (status_channel.h), so there is no second set of auth or validation rules to
// get wrong.
//
// It exists because the LAN path is the fragile one: a VLAN boundary, a blocked
// mDNS, or an AP that associates and then carries nothing (docs/network-polling.md)
// all break an HTTP request — and none of them mean anything to BLE, which never
// touches IP. The transport ladder lives in the client (tools/opencode-plugin):
// LAN first, this second, and nothing after that.
//
// **Scope: only what the PC initiates.** State pings and settings requests come
// from the host, so they can arrive over either transport. usage/weather/moon/air
// are fetches *this* device makes out to the internet and still need WiFi — BLE
// cannot stand in for them, and nothing here tries to.
//
// **Pairing: none, deliberately.** The service holds no encrypted characteristics
// and no bonding is requested, so a macOS app connects and writes without the
// system ever showing a pairing dialog. What bonding would have bought — "only
// my host can drive this" — is already provided at the application layer by the
// same HMAC-SHA256 the HTTP path verifies, and the payload carries that signature
// rather than relying on the link. Requiring encryption here would mean requiring
// bonding, which is the pairing ceremony this whole approach avoids.
//
// Threading: the stack calls back on its own (BTC) task, which must not touch
// flash. An incoming write is only *copied* there; bleStatusUpdate() handles it
// on the loop task, exactly like an HTTP request, so a settings write still lands
// on the one task allowed to write LittleFS.

// Bring up the GATT service and start advertising. `enable` is the `ble_status`
// config toggle; a disabled peripheral never initialises the stack at all, so
// there is no hidden cost for a device that does not want this.
//
// Call once from setup(), after statusChannelSetKey() /
// statusChannelSetSettingsCallback() — the channel this serves must be configured
// before it can answer anything.
void bleStatusBegin(bool enable);

// Pump it from loop(): handle a pending write, keep the read-only info
// characteristic current, and drop a link that has gone silent. Never blocks
// beyond one notification.
void bleStatusUpdate();

// What the peripheral actually achieved. `UP` deliberately does **not** mean
// "advertising": the library keeps no advertising state and offers nothing to
// query (its GAP handlers are empty, BLEAdvertising.cpp:527), so a scan from the
// PC — tools/ble-bridge.py --scan — stays the only real test.
enum BleState : uint8_t {
  BLE_STATE_OFF = 0,   // ble_status is false in config.json
  BLE_STATE_FAILED,    // bring-up bailed; the Serial line it printed says why
  BLE_STATE_UP,        // controller running, GATT service registered
};

BleState bleStatusState();

// "off" / "failed" / "up", for the periodic diag line (src/diag.cpp).
const char* bleStatusStateName();

// The 128-bit UUID a client scans for to find this service.
#define BLE_STATUS_SERVICE_UUID "6f4b1e00-9a3c-4d5e-8f21-7c0d9e5a4b30"
