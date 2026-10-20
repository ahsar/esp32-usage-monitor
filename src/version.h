#pragma once

// Firmware version, surfaced in the boot log (and available for future use in
// the status server / OTA handshake). Keep in sync with CHANGELOG.md.
//
// 0.4.0: the device channel over BLE (src/ble_status.cpp) and the LAN address on
// the clock page. Bumped deliberately — the boot banner is how you tell which
// firmware is actually running, and "BLE is quietly not advertising" is exactly
// the case where that matters.
//
// 0.5.0: the agent-context request (src/parse/agent_parse.h), the Usage page's
// spend/pace/OVER and its threshold cues, and the Health page. The agent kind is
// the one that matters for version skew: an older firmware parses `?agent=…` as
// the settings channel and answers 400, so the plugin needs this build.
#define FW_VERSION "0.5.0"
