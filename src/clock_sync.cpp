#include "clock_sync.h"
#include "hw/rtc.h"
#include "util/time_util.h"
#include <Arduino.h>
#include <time.h>

static bool     s_started = false;
static bool     s_valid   = false;
static uint32_t s_lastSync = 0;

static void doSync() {
  if (!s_started) {
    configTime(8 * 3600, 0, "ntp.aliyun.com", "pool.ntp.org");
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
    s_valid = true;
  }
}

bool clockSyncValid() { return s_valid; }

void clockSyncUpdate(uint32_t now, bool force) {
  uint32_t interval = s_valid ? 3600000u : 5000u;
  if (force || now - s_lastSync >= interval) {
    s_lastSync = now;
    doSync();
  }
}
