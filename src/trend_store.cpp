#include "trend_store.h"
#include "usage_history.h"
#include "util/trend_store_util.h"
#include "util/time_util.h"
#include "util/usage_pace.h"

#include <LittleFS.h>
#include <Arduino.h>
#include <string.h>

// The virtual offset for the trend timeline. It only has to be larger than the
// oldest sample we could ever restore (the rolling window), so a restored point
// stays positive: `base - age` with age ≤ 5 h.
static const uint32_t TREND_BASE_SEC = usage_pace::WINDOW_5H_SEC + 120;

static const char* TREND_PATH = "/trend.txt";

// 0 until trendStoreRestore() runs; a push before that just uses the boot clock,
// which is what the ring did before this module existed.
static uint32_t s_base = 0;

uint32_t trendNowSec(uint32_t monoSec) { return s_base + monoSec; }

// Read the header line into `out`, returning false when it is not ours.
static bool readHeader(File& f, char* out, size_t cap) {
  if (!cap) return false;
  size_t n = f.readBytesUntil('\n', out, cap - 1);
  out[n] = '\0';
  while (n > 0 && (out[n - 1] == '\r' || out[n - 1] == ' ')) out[--n] = '\0';
  return n > 0 && !strcmp(out, TREND_STORE_MAGIC);
}

int trendStoreRestore(uint32_t nowEpoch, uint32_t monoSec) {
  s_base = TREND_BASE_SEC;
  if (nowEpoch < CLOCK_VALID_MIN) return 0;

  File f = LittleFS.open(TREND_PATH, "r");
  if (!f) return 0;

  char hdr[32];
  if (!readHeader(f, hdr, sizeof(hdr))) {
    f.close();
    return 0;
  }

  int restored = 0;
  char line[96];
  while (restored < TREND_STORE_MAX && f.available()) {
    size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = '\0';
    while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = '\0';

    TrendRecord r;
    if (!trendStoreParseLine(line, r)) break;   // truncated tail: keep the prefix

    // A sample from the future means the clock moved backwards (NTP correction,
    // or a backup battery that drifted). Treat it as "now" rather than dropping
    // the whole history over it.
    if (r.epoch > nowEpoch) r.epoch = nowEpoch;
    uint32_t age = nowEpoch - r.epoch;
    // Older than the rolling window: it belongs to a window that has already
    // reset, and plotting it next to today's spend would be a lie.
    if (age > usage_pace::WINDOW_5H_SEC) continue;

    trendPush((s_base - age) + monoSec, r.usedMicroCents, r.pct);
    restored++;
  }
  f.close();

  if (restored) Serial.printf("trend: restored %d samples from %s\n", restored, TREND_PATH);
  return restored;
}

bool trendStoreSave(uint32_t nowEpoch, uint32_t monoSec) {
  int n = trendCount();
  if (n <= 0 || nowEpoch < CLOCK_VALID_MIN) return false;

  File f = LittleFS.open(TREND_PATH, "w");
  if (!f) {
    Serial.println("trend: cannot open for write");
    return false;
  }

  f.printf("%s\n", TREND_STORE_MAGIC);

  const uint32_t nowV = trendNowSec(monoSec);
  char line[96];
  int written = 0;
  for (int i = 0; i < n; i++) {
    const TrendPoint& p = trendAt(i);
    // A point can only be newer than "now" if the caller mixed timelines; skip
    // it rather than writing an epoch in the future.
    if ((int32_t)(nowV - p.t) < 0) continue;
    TrendRecord r;
    r.epoch          = nowEpoch - (nowV - p.t);
    r.usedMicroCents = p.usedMicroCents;
    r.pct            = p.pct;
    if (!trendStoreLine(r, line, sizeof(line))) continue;
    f.printf("%s\n", line);
    written++;
  }
  f.close();

  if (!written) {
    LittleFS.remove(TREND_PATH);
    return false;
  }
  return true;
}
