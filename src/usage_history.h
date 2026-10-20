#pragma once
#include <stdint.h>

// Recent samples of the rolling (5-hour) meter, for the trend page.
//
// One sample per successful usage poll, so 120 samples is ~2 h at the active
// 60 s cadence and ~6 h when idle. The ring itself is RAM; trend_store.cpp
// persists it to /trend.txt (wall-clock epochs) in between, so a reboot or an
// OTA restores the curve instead of blanking the page. Nothing here touches
// flash: the ingest runs from serviceFetch() on the loop task.
struct TrendPoint {
  uint32_t t;               // seconds on the trend timeline (see trend_store.h)
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

// Spend rate over the whole retained window, in microcents/hour (0 when there
// are fewer than two samples, or the window reset inside it). Both the Trend
// page and the Usage page's pace verdict read the rate from here, so the two
// cannot drift apart — and neither one derives it from a single poll interval,
// which would be wild after a slow one.
uint64_t trendBurnPerHour();
