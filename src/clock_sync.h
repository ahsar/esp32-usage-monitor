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

// Seed the system clock from the PCF85063 at boot, before the first TLS fetch.
// The RTC holds Beijing wall time (clock_sync writes it with the UTC+8 offset
// configTime() installs), so the offset is subtracted here. No-op when the RTC
// is unset or implausible (e.g. a dead backup battery).
void clockSyncRestoreFromRtc();

// Call from the desktop loop. `force` syncs immediately (e.g. right after WiFi
// connects) regardless of the interval.
void clockSyncUpdate(uint32_t now, bool force = false);
