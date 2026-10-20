#pragma once
// "Will this meter run out before its window resets?" — the pure math behind the
// Usage page's OVER tag. Header-only and hardware-free so the host tests can
// cover it.
//
// Two models, because the two kinds of meter know different things:
//
//  - the five-hour meter is the only one `usage_history` samples (one point per
//    usage poll, ~2 h at the active cadence), so its verdict is a real
//    projection: spend + (measured rate × time to reset);
//  - week/month have no measured rate on this device, and projecting the 5-hour
//    rate onto them would be a fiction. They get the *even-burn* model instead:
//    compare what is spent with how much of the window has elapsed. That assumes
//    spending is roughly uniform across the window — a stated assumption, not a
//    measurement — and it catches the thing that actually hurts ("70% of the
//    weekly limit gone on Tuesday") without pretending to know the future.
#include <stdint.h>

namespace usage_pace {

// Meter window lengths. The console only reports `resetsAt`, so the length is
// what turns a countdown into "fraction of the window already spent".
//
// The month is the awkward one: the console's window is a calendar-ish billing
// month (28–31 days) and `resetsAt` does not say which. 31 is the longest, so the
// countdown always fits inside it — the alternative (30) makes a 31-day month
// report no verdict at all for its first day. The even line is then at most ~3 %
// early for a 28-day month, well inside the verdict's slack.
constexpr uint32_t WINDOW_5H_SEC    = 5u * 3600u;
constexpr uint32_t WINDOW_WEEK_SEC  = 7u * 86400u;
constexpr uint32_t WINDOW_MONTH_SEC = 31u * 86400u;

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

// How much of the meter's window has elapsed, in percent (0..100).
//
// `resetSec` is the countdown the console reported. A countdown longer than the
// window means the two disagree (a clock jump, or a window length this firmware
// does not know) — answer 0 rather than a negative "elapsed".
inline int elapsedPct(uint32_t windowSec, uint32_t resetSec) {
  if (windowSec == 0 || resetSec >= windowSec) return 0;
  return (int)(((uint64_t)(windowSec - resetSec) * 100u) / windowSec);
}

// Even-burn verdict for a meter with no measured rate.
//
// `slackPct` is headroom before the tag: a meter a little above the even line is
// not worth a warning, and the weekly/monthly windows are long enough that a
// handful of points is noise. Ten points is a third of a day on a weekly meter.
//
// A countdown of exactly 0 is "the window is resetting right now", which is the
// same unknown the 5-hour model treats as UNKNOWN: the percentage on screen may
// still be the old window's.
inline Verdict evenVerdict(int usedPct, uint32_t windowSec, uint32_t resetSec,
                           int slackPct = 10) {
  if (windowSec == 0 || resetSec == 0 || resetSec > windowSec) return PACE_UNKNOWN;
  if (usedPct < 0) usedPct = 0;
  return usedPct > elapsedPct(windowSec, resetSec) + slackPct ? PACE_OVER
                                                              : PACE_ON_TRACK;
}

}  // namespace usage_pace
