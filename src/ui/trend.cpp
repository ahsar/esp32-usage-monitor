// Trend page: rolling-window spend, its rate, and two recent-history plots
// (cumulative spend, and the per-poll increment) built from usage_history.
#include "ui/ui_internal.h"
#include "usage_display.h"
#include "ui/theme.h"
#include "usage_history.h"
#include "util/trend_util.h"
#include <cstdio>

using namespace ui_theme;

namespace {

// Plot geometry (logical units).
const int CURVE_LABEL_Y = 98;
const int CURVE_TOP     = 104;
const int CURVE_BOT     = 146;
const int RATE_LABEL_Y  = 158;
const int BAR_BOT       = 198;
const int BAR_MAX_H     = 34;

const int CURVE_POINTS = 60;   // newest samples shown as a line
const int BAR_SLOTS    = 40;   // newest per-poll increments shown as bars

// Newest increment minus the one before it (0 when the meter reset).
uint64_t deltaAt(int i) {
  uint64_t a = trendAt(i - 1).usedMicroCents;
  uint64_t b = trendAt(i).usedMicroCents;
  return b > a ? b - a : 0;
}

void drawCurve(int n) {
  textAt(F_SMALL, COL_MUTED, SAFE_L, CURVE_LABEL_Y, "spend");
  if (n < 2) {
    textCenter(F_SMALL, COL_MUTED, HW_W / 2, CURVE_TOP + 20, "collecting...");
    return;
  }

  int start = n > CURVE_POINTS ? n - CURVE_POINTS : 0;
  int m = n - start;

  uint64_t lo = trendAt(start).usedMicroCents, hi = lo;
  for (int i = start + 1; i < n; i++) {
    uint64_t v = trendAt(i).usedMicroCents;
    if (v < lo) lo = v;
    if (v > hi) hi = v;
  }
  if (hi == lo) hi = lo + 1;

  const uint64_t span = hi - lo;
  const int h = CURVE_BOT - CURVE_TOP;
  int px = -1, py = 0;
  for (int i = start; i < n; i++) {
    int x = SAFE_L + (int)((long)(i - start) * (SAFE_W - 1) / (m - 1));
    int y = CURVE_BOT - (int)((trendAt(i).usedMicroCents - lo) * (uint64_t)h / span);
    if (px >= 0) spr.drawLine(X(px), Y(py), X(x), Y(y), COL_GREEN);
    px = x; py = y;
  }
  spr.drawFastHLine(X(SAFE_L), Y(CURVE_BOT + 1), SAFE_W, COL_TRACK);
}

void drawBars(int n) {
  textAt(F_SMALL, COL_MUTED, SAFE_L, RATE_LABEL_Y, "per poll");
  if (n < 2) return;

  int start = n > BAR_SLOTS + 1 ? n - (BAR_SLOTS + 1) : 1;
  int m = n - start;
  if (m < 1) return;

  uint64_t maxd = 0;
  for (int i = start; i < n; i++) {
    uint64_t d = deltaAt(i);
    if (d > maxd) maxd = d;
  }
  if (maxd == 0) maxd = 1;

  int bw = SAFE_W / m;
  if (bw < 1) bw = 1;
  for (int i = start; i < n; i++) {
    uint64_t d = deltaAt(i);
    int bh = (int)(d * (uint64_t)BAR_MAX_H / maxd);
    if (bh == 0 && d > 0) bh = 1;
    if (bh == 0) continue;
    spr.fillRect(X(SAFE_L + (i - start) * bw), Y(BAR_BOT - bh),
                 bw > 1 ? bw - 1 : 1, bh, COL_ORANGE);
  }
  // A reset between the last two polls breaks the accumulation; mark it.
  if (trendLastWasReset()) {
    textRight(F_SMALL, COL_YELLOW, SAFE_R, RATE_LABEL_Y, "reset");
  }
}

}  // namespace

void uiDrawTrend(const UsageData& data, int ox, int oy) {
  g_ox = ox; g_oy = oy;
  cardBg();
  statusBar("Trend");

  const int n = trendCount();
  const bool haveCost = data.valid &&
                        (data.rollingUsedMicroCents || data.rollingLimitMicroCents);

  char buf[32];
  if (haveCost) trend::formatUsd(buf, sizeof(buf), data.rollingUsedMicroCents);
  else          snprintf(buf, sizeof(buf), "--");
  textCenter(F_BIG, COL_TEXT, HW_W / 2, 64, buf);

  // Rate + ETA come from the retained window, not a single poll, so one slow
  // interval doesn't produce a wild number.
  if (haveCost) {
    uint64_t burn = 0;
    if (n >= 2) {
      const TrendPoint& a = trendAt(0);
      const TrendPoint& b = trendAt(n - 1);
      uint64_t delta = b.usedMicroCents > a.usedMicroCents
                           ? b.usedMicroCents - a.usedMicroCents : 0;
      burn = trend::burnPerHour(delta, b.t - a.t);
    }

    char rate[24];
    trend::formatBurn(rate, sizeof(rate), burn);
    textAt(F_SMALL, COL_DIM, SAFE_L, 84, rate);

    uint64_t remaining = data.rollingLimitMicroCents > data.rollingUsedMicroCents
                             ? data.rollingLimitMicroCents - data.rollingUsedMicroCents : 0;
    char eta[20], line[40];
    trend::formatEta(eta, sizeof(eta), trend::etaSeconds(remaining, burn));
    snprintf(line, sizeof(line), "limit in %s", eta);
    textRight(F_SMALL, COL_MUTED, SAFE_R, 84, line);
  } else {
    textCenter(F_BODY, COL_MUTED, HW_W / 2, 84, "no data yet");
  }

  spr.drawFastHLine(X(SAFE_L), Y(88), SAFE_W, COL_LINE);

  drawCurve(n);
  drawBars(n);
}
