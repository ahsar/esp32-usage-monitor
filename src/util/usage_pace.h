#pragma once
// "Will this meter run out before its window resets?" — the pure math behind the
// Usage page's OVER tag. Header-only and hardware-free so the host tests can
// cover it.
//
// Only the five-hour meter gets a verdict. The projection is spend plus
// (rate × time to reset), and the rate the device can actually measure is the
// rolling meter's: it is the only one `usage_history` samples (one point per
// usage poll, ~2 h of history at the active cadence). Applying that same rate to
// the weekly/monthly counts would be a fiction, so those two are displayed as
// spend only.
#include <stdint.h>

namespace usage_pace {

enum Verdict : uint8_t {
  PACE_UNKNOWN = 0,   // no burn rate yet (fresh boot), or the window is resetting
  PACE_ON_TRACK,      // projected to stay under the limit until the reset
  PACE_OVER,          // projected to exceed the limit before the reset
};

// Spend projected at the moment the window resets, holding the current rate.
inline uint64_t projectedAtReset(uint64_t usedMicroCents,
                                 uint64_t burnPerHourMicroCents,
                                 uint32_t resetSec) {
  return usedMicroCents + burnPerHourMicroCents * (uint64_t)resetSec / 3600ULL;
}

// A zero burn is "we have not measured a rate yet" (the RAM history is empty
// after a reboot), never "safely spending nothing" — reporting that as ON_TRACK
// would be the reassuring answer at exactly the moment it is least earned.
inline Verdict verdict(uint64_t usedMicroCents, uint64_t limitMicroCents,
                       uint64_t burnPerHourMicroCents, uint32_t resetSec) {
  if (limitMicroCents == 0 || burnPerHourMicroCents == 0 || resetSec == 0)
    return PACE_UNKNOWN;
  return projectedAtReset(usedMicroCents, burnPerHourMicroCents, resetSec) > limitMicroCents
             ? PACE_OVER
             : PACE_ON_TRACK;
}

}  // namespace usage_pace
