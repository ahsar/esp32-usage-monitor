#pragma once
#include <stdint.h>

// Persistence for the trend ring (usage_history), so the Trend page survives a
// reboot, an OTA and a power cycle.
//
// The RAM ring samples the *rolling 5-hour* meter, so its timeline is
// boot-relative (`millis()`); after a reboot those numbers mean nothing. The
// file therefore stores wall-clock epochs, and this module owns the translation:
//
//   trendNowSec(monoSec)  the virtual clock the ring stores. Restored samples
//                         keep their spacing, and the first new sample lands
//                         exactly one poll interval after the last restored one.
//
// Everything here is loop-task only (LittleFS), like settingsSave() and the
// token flush. Restoring is best-effort: a missing file, a corrupt tail or an
// unset clock just leaves the Trend page collecting from scratch, which is
// exactly the behaviour before this existed.
//
// **Call trendStoreRestore() before the first trendPush()**, and only after
// clockSyncRestoreFromRtc() — the file is pruned to the samples that could still
// belong to the *current* rolling window, and that needs the date.

// Restore the persisted samples into usage_history. `nowEpoch` is time(nullptr)
// (0 when the clock is not plausible); `monoSec` is millis()/1000. Returns how
// many points were restored.
int trendStoreRestore(uint32_t nowEpoch, uint32_t monoSec);

// Write the current ring. Returns false when there was nothing to write, the
// clock is unset, or the file could not be opened. Called at most every few
// minutes (see main.cpp) and on the way into a power-off/OTA reboot.
bool trendStoreSave(uint32_t nowEpoch, uint32_t monoSec);

// The virtual trend clock, in seconds. Feed this to trendPush() so restored and
// fresh samples share one timeline.
uint32_t trendNowSec(uint32_t monoSec);
