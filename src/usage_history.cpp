#include "usage_history.h"
#include <string.h>

static TrendPoint s_pts[TREND_MAX];
static int        s_count   = 0;
static bool       s_lastReset = false;

void trendReset() {
  s_count = 0;
  s_lastReset = false;
}

void trendPush(uint32_t t, uint64_t usedMicroCents, int pct) {
  if (s_count > 0) {
    // Spend only grows within a window, so a drop means the meter reset.
    // Record the discontinuity so the curve can break instead of drawing a
    // vertical line down to zero.
    s_lastReset = usedMicroCents < s_pts[s_count - 1].usedMicroCents;
  } else {
    s_lastReset = false;
  }

  if (s_count == TREND_MAX) {
    memmove(&s_pts[0], &s_pts[1], (TREND_MAX - 1) * sizeof(TrendPoint));
    s_count--;
  }
  s_pts[s_count].t              = t;
  s_pts[s_count].usedMicroCents = usedMicroCents;
  s_pts[s_count].pct            = (uint8_t)(pct < 0 ? 0 : (pct > 100 ? 100 : pct));
  s_count++;
}

int trendCount() { return s_count; }

const TrendPoint& trendAt(int i) {
  static const TrendPoint zero = {};
  if (i < 0 || i >= s_count) return zero;
  return s_pts[i];
}

bool trendLastWasReset() { return s_lastReset; }
