#pragma once
#include <stdint.h>

// NTP bootstrapping + RTC resync scheduling.
//
// configTime() is issued once, then the RTC is re-synced hourly (every 5 s while
// the clock is still invalid, so it converges quickly after WiFi comes up).

bool clockSyncValid();

// Call from the desktop loop. `force` syncs immediately (e.g. right after WiFi
// connects) regardless of the interval.
void clockSyncUpdate(uint32_t now, bool force = false);
