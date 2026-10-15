#include "clock_sync.h"
#include "hw/rtc.h"
#include "util/time_util.h"
#include <Arduino.h>
#include <sys/time.h>
#include <time.h>

// The NTP/display timezone, in seconds. Set from config (clockSyncSetOffset())
// before the first sync. The RTC is written in local wall time, so seeding the
// system clock from it has to subtract this to get UTC epoch seconds.
static int s_tzOffsetSec = 8 * 3600;

static bool     s_started = false;
static bool     s_valid   = false;   // plausible wall clock (RTC or NTP)
static bool     s_ntp     = false;   // NTP has answered at least once
static uint32_t s_lastSync = 0;

void clockSyncSetOffset(int tzOffsetMin) {
  const int MAX = 16 * 60;   // widest real-world zone
  if (tzOffsetMin > MAX)  tzOffsetMin = MAX;
  if (tzOffsetMin < -MAX) tzOffsetMin = -MAX;
  s_tzOffsetSec = tzOffsetMin * 60;
  // configTime() was already installed from the previous value; re-issuing it on
  // the next doSync() keeps TZ and the RTC restore in step.
  s_started = false;
}

static void doSync() {
  if (!s_started) {
    configTime(s_tzOffsetSec, 0, "ntp.aliyun.com", "pool.ntp.org");
    s_started = true;
  }
  time_t ts = time(nullptr);
  if (ts > (time_t)CLOCK_VALID_MIN) {
    struct tm lt;
    localtime_r(&ts, &lt);
    HwTime rt = {
      (uint8_t)lt.tm_hour, (uint8_t)lt.tm_min, (uint8_t)lt.tm_sec,
      (uint16_t)(lt.tm_year + 1900),
      (uint8_t)(lt.tm_mon + 1), (uint8_t)lt.tm_mday, (uint8_t)lt.tm_wday
    };
    hwRtcWrite(rt);
    s_ntp = true;
    s_valid = true;
  }
}

void clockSyncRestoreFromRtc() {
  HwTime t;
  if (!hwRtcRead(&t)) return;

  uint32_t e = civilToEpoch(t.Y, t.Mo, t.D, t.H, t.M, t.S);
  if (e < CLOCK_VALID_MIN) return;   // unset / dead backup battery

  e -= (uint32_t)s_tzOffsetSec;
  struct timeval tv = { (time_t)e, 0 };
  settimeofday(&tv, nullptr);
  s_valid = true;
  Serial.printf("clock: restored from RTC (%04u-%02u-%02u %02u:%02u)\n",
                (unsigned)t.Y, (unsigned)t.Mo, (unsigned)t.D,
                (unsigned)t.H, (unsigned)t.M);
}

bool clockSyncValid() { return s_valid; }

void clockSyncUpdate(uint32_t now, bool force) {
  // Keep retrying every 5 s until NTP actually answers, even when the RTC
  // already gave us a usable clock — the RTC drifts and only NTP corrects it.
  uint32_t interval = s_ntp ? 3600000u : 5000u;
  if (force || now - s_lastSync >= interval) {
    s_lastSync = now;
    doSync();
  }
}
