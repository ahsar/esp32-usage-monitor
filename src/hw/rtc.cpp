#include "hw/rtc.h"
#include "hw/pins.h"
#include <Wire.h>

#if BOARD_HAS_PCF85063

#include <SensorPCF85063.hpp>

static SensorPCF85063 s_pcf;

bool hwRtcInit() {
  if (!s_pcf.begin(Wire, PIN_I2C_SDA, PIN_I2C_SCL)) {
    Serial.println("hwRtc: PCF85063 begin failed");
    return false;
  }
  return true;
}

bool hwRtcRead(HwTime* t) {
  RTC_DateTime dt = s_pcf.getDateTime();
  t->H   = dt.getHour();
  t->M   = dt.getMinute();
  t->S   = dt.getSecond();
  t->Y   = dt.getYear();
  t->Mo  = dt.getMonth();
  t->D   = dt.getDay();
  t->dow = dt.getWeek();
  return true;
}

bool hwRtcWrite(const HwTime& t) {
  RTC_DateTime dt(t.Y, t.Mo, t.D, t.H, t.M, t.S, t.dow);
  s_pcf.setDateTime(dt);
  return true;
}

#else   // No external RTC — software clock, synced by BLE bridge on connect.

#include <time.h>

static time_t   s_epoch  = 0;
static uint32_t s_syncMs = 0;
static bool     s_synced = false;

bool hwRtcInit() { return true; }

bool hwRtcWrite(const HwTime& t) {
  struct tm lt = {};
  lt.tm_sec  = t.S;
  lt.tm_min  = t.M;
  lt.tm_hour = t.H;
  lt.tm_mday = t.D;
  lt.tm_mon  = t.Mo - 1;
  lt.tm_year = t.Y - 1900;
  s_epoch = mktime(&lt);
  if (s_epoch == (time_t)-1) return false;
  s_syncMs = millis();
  s_synced = true;
  return true;
}

bool hwRtcRead(HwTime* t) {
  if (!s_synced) { *t = {}; return false; }
  // uint32_t subtraction wraps correctly for spans <49.7 days.
  uint32_t elapsed = (millis() - s_syncMs) / 1000;
  time_t now = s_epoch + (time_t)elapsed;
  struct tm lt;
  localtime_r(&now, &lt);
  t->S   = lt.tm_sec;
  t->M   = lt.tm_min;
  t->H   = lt.tm_hour;
  t->D   = lt.tm_mday;
  t->Mo  = lt.tm_mon + 1;
  t->Y   = lt.tm_year + 1900;
  t->dow = lt.tm_wday;
  return true;
}

#endif
