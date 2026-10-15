#pragma once
#include <stdint.h>

// NTP bootstrapping + RTC resync scheduling.
//
// configTime() is issued once, then the RTC is re-synced hourly (every 5 s while
// NTP has not answered yet, so it converges quickly after WiFi comes up).
//
// `clockSyncValid()` means "we have a plausible wall clock", which is what the
// TLS certificate date check needs. It becomes true either from an NTP answer or
// from clockSyncRestoreFromRtc(), so the network stack is usable even if NTP is
// blocked on the local network.

bool clockSyncValid();

// Install the display/NTP timezone (minutes east of UTC, clamped to ±16 h). Call
// once from config, BEFORE clockSyncRestoreFromRtc()/clockSyncUpdate(): the RTC
// holds local wall time, so this is the offset that turns it back into UTC. It
// also re-arms configTime() so the two cannot disagree.
void clockSyncSetOffset(int tzOffsetMin);

// Seed the system clock from the PCF85063 at boot, before the first TLS fetch.
// The RTC holds local wall time (clock_sync writes it with the offset from
// clockSyncSetOffset()), so that offset is subtracted here. No-op when the RTC
// is unset or implausible (e.g. a dead backup battery).
void clockSyncRestoreFromRtc();

// Call from the desktop loop. `force` syncs immediately (e.g. right after WiFi
// connects) regardless of the interval.
void clockSyncUpdate(uint32_t now, bool force = false);
