#pragma once
#include <stdint.h>

// Recent samples of the rolling (5-hour) meter, for the trend page.
//
// RAM only: the history is a "what has this window been doing lately" view, so
// losing it on reboot is fine and it keeps flash writes off the hot path (the
// ingest runs from serviceFetch() every usage poll). One sample per poll, so
// 120 samples is ~2 h at the active 60 s cadence and ~6 h when idle.
struct TrendPoint {
  uint32_t t;               // seconds, monotonic since boot
  uint64_t usedMicroCents;  // rolling-window spend at that instant
  uint8_t  pct;             // rolling-window percent
};

static const int TREND_MAX = 120;

void trendReset();
void trendPush(uint32_t t, uint64_t usedMicroCents, int pct);

int trendCount();
// 0 = oldest retained sample. Invalid index returns a zeroed point.
const TrendPoint& trendAt(int i);

// True when the newest sample's spend is below the previous one, i.e. the
// window reset between the last two polls (the curve has a discontinuity).
bool trendLastWasReset();
