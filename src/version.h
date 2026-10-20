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
//
// 0.6.0: the trust/data batch — usage data age + tap-to-refresh, even-burn
// verdicts for the weekly/monthly meters, a persisted trend ring, concurrent
// agent sessions, the Notice page (`?notice=…`) and the three-day forecast card.
// Two of those change the wire: `?agent=` gained `session=`, and `?notice=` is a
// new kind an older firmware would answer 400 (it is the settings branch). The
// session field is optional, so an older plugin still works; the reverse (a new
// plugin on 0.5.0) loses only the multi-session aggregation.
#define FW_VERSION "0.6.0"
