#pragma once

// Firmware version, surfaced in the boot log (and available for future use in
// the status server / OTA handshake). Keep in sync with CHANGELOG.md.
//
// 0.4.0: the device channel over BLE (src/ble_status.cpp) and the LAN address on
// the clock page. Bumped deliberately — the boot banner is how you tell which
// firmware is actually running, and "BLE is quietly not advertising" is exactly
// the case where that matters.
#define FW_VERSION "0.4.0"
