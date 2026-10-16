// UI cards: usage / weather / clock pages plus the agent-activity animations.
#include "ui/ui_internal.h"
#include "usage_display.h"
#include "ui/theme.h"
#include "util/value_util.h"
#include <cstdio>
#include <cstring>
#include <cmath>

using namespace ui_theme;

// Weather cards keep this right margin free for the vertical card dots.
static const int WC_R = SAFE_R - 12;

// ---------------------------------------------------------------------------
// USAGE card
// ---------------------------------------------------------------------------

static void fmtReset(char* buf, size_t sz, uint32_t sec) {
  uint32_t d = sec / 86400; sec %= 86400;
  uint32_t h = sec / 3600;  sec %= 3600;
  uint32_t m = sec / 60;
  if (d > 0)      snprintf(buf, sz, "%lud %luh", (unsigned long)d, (unsigned long)h);
  else if (h > 0) snprintf(buf, sz, "%luh %lum", (unsigned long)h, (unsigned long)m);
  else            snprintf(buf, sz, "%lum", (unsigned long)m);
}

void uiDrawUsage(const UsageData& data, int ox, int oy) {
  g_ox = ox; g_oy = oy;
  cardBg();
  statusBar("Usage");

  struct Row { const char* label; int pct; uint32_t reset; };
  Row rows[3] = {
    { "5-HOUR",  data.rollingPercent, data.rollingResetSec },
    { "WEEKLY",  data.weeklyPercent,  data.weeklyResetSec  },
    { "MONTHLY", data.monthlyPercent, data.monthlyResetSec },
  };

  for (int i = 0; i < 3; i++) {
    int top = 44 + i * 52;
    bool ok = data.valid;
    uint16_t col = ok ? usageColor(rows[i].pct) : COL_MUTED;

    textAt(F_HEAD, COL_DIM, SAFE_L, top + 10, rows[i].label);

    char pct[8];
    if (ok) snprintf(pct, sizeof(pct), "%d%%", rows[i].pct);
    else    snprintf(pct, sizeof(pct), "--");
    textRight(F_MED, col, SAFE_R, top + 20, pct);

    roundBar(SAFE_L, top + 26, SAFE_W, 14, ok ? rows[i].pct : 0, col);

    char reset[24];
    if (!ok) { snprintf(reset, sizeof(reset), "no data"); }
    else if (rows[i].reset == 0) { snprintf(reset, sizeof(reset), "resetting soon"); }
    else {
      char t[16];
      fmtReset(t, sizeof(t), rows[i].reset);
      snprintf(reset, sizeof(reset), "resets in %s", t);
    }
    textRight(F_SMALL, COL_MUTED, SAFE_R, top + 50, reset);
  }
}

// ---------------------------------------------------------------------------
// WEATHER cards
// ---------------------------------------------------------------------------

static void wcNow(const WeatherData& w) {
  statusBar(g_env.city && g_env.city[0] ? g_env.city : "Weather");

  if (!w.valid) {
    textCenter(F_HEAD, COL_MUTED, HW_W / 2, 110, "no weather data");
    return;
  }

  drawWeatherIcon(kindFromText(w.condition), 40, 72);

  char t[12];
  snprintf(t, sizeof(t), "%d\u00B0", w.tempC);
  textAt(F_BIG, COL_TEXT, 84, 78, t);

  textAt(F_BODY, COL_DIM, 86, 100, w.condition);
  if (w.stale) textAt(F_SMALL, COL_MUTED, 86, 114, "STALE");

  spr.drawFastHLine(X(SAFE_L), Y(122), WC_R - SAFE_L, COL_LINE);

  char val[32];
  int y = 138;
  const int dy = 16;

  textAt(F_BODY, COL_DIM, SAFE_L, y, "Feels");
  snprintf(val, sizeof(val), "%d\u00B0", w.feelsC);
  textRight(F_BODY, COL_TEXT, WC_R, y, val);
  y += dy;

  textAt(F_BODY, COL_DIM, SAFE_L, y, "Humidity");
  snprintf(val, sizeof(val), "%d%%", w.humidity);
  textRight(F_BODY, COL_TEXT, WC_R, y, val);
  y += dy;

  textAt(F_BODY, COL_DIM, SAFE_L, y, "Wind");
  snprintf(val, sizeof(val), "%d km/h %s", w.windKmph, w.windDir);
  drawWindArrow(WC_R - textW(F_BODY, val) - 9, y - 4, w.windDir, COL_DIM);
  textRight(F_BODY, COL_TEXT, WC_R, y, val);
  y += dy;

  textAt(F_BODY, COL_DIM, SAFE_L, y, "Precip");
  snprintf(val, sizeof(val), "%.1f mm", w.precipTenths / 10.0f);
  textRight(F_BODY, COL_TEXT, WC_R, y, val);

  if (w.obsTime[0]) {
    char buf[32];
    snprintf(buf, sizeof(buf), "Updated %s", w.obsTime);
    textAt(F_SMALL, COL_MUTED, SAFE_L, y + 18, buf);
  }
}

static void fmtClock(char* buf, size_t sz, int mins) {
  if (mins < 0) { snprintf(buf, sz, "--:--"); return; }
  snprintf(buf, sz, "%02d:%02d", mins / 60, mins % 60);
}

static void wcSun(const WeatherData& w) {
  statusBar("Sun");

  char t[8];
  int y = 92;

  textAt(F_BODY, COL_DIM, SAFE_L, y, "Sunrise");
  fmtClock(t, sizeof(t), w.valid ? w.sunriseMin : -1);
  textRight(F_MED, COL_SUN, WC_R, y, t);

  y += 36;
  textAt(F_BODY, COL_DIM, SAFE_L, y, "Sunset");
  fmtClock(t, sizeof(t), w.valid ? w.sunsetMin : -1);
  textRight(F_MED, COL_ORANGE, WC_R, y, t);

  spr.drawFastHLine(X(SAFE_L), Y(152), WC_R - SAFE_L, COL_LINE);

  y = 180;
  textAt(F_BODY, COL_DIM, SAFE_L, y, "Daylight");
  if (w.valid && w.sunriseMin >= 0 && w.sunsetMin >= 0) {
    int mins = w.sunsetMin - w.sunriseMin;
    if (mins < 0) mins += 24 * 60;
    char d[16];
    snprintf(d, sizeof(d), "%dh %02dm", mins / 60, mins % 60);
    textRight(F_MED, COL_TEXT, WC_R, y, d);
  } else {
    textRight(F_MED, COL_MUTED, WC_R, y, "--");
  }
}

static void wcAir(const AirData& a, bool hasToken) {
  statusBar("Air");

  if (!hasToken) {
    textCenter(F_HEAD, COL_YELLOW, HW_W / 2, 96, "AUTH NEEDED");
    textCenter(F_SMALL, COL_MUTED, HW_W / 2, 118, "set waqi_token");
    textCenter(F_SMALL, COL_MUTED, HW_W / 2, 132, "in config.json");
    return;
  }
  if (!a.valid) {
    textCenter(F_HEAD, COL_MUTED, HW_W / 2, 110, "no air data");
    return;
  }

  uint16_t col = aqiColor(a.aqi);

  if (g_env.airLabel && g_env.airLabel[0])
    textCenter(F_SMALL, COL_MUTED, HW_W / 2, 54, g_env.airLabel);

  char buf[24];
  if (a.aqi >= 0) snprintf(buf, sizeof(buf), "%d", a.aqi);
  else            snprintf(buf, sizeof(buf), "--");
  textCenter(F_BIG, col, HW_W / 2, 96, buf);

  textCenter(F_HEAD, col, HW_W / 2, 122, aqiCategory(a.aqi));

  int pct = (a.aqi < 0) ? 0 : (a.aqi * 100 / 300);
  roundBar(SAFE_L, 134, WC_R - SAFE_L, 14, pct, col);

  if (a.pm25 >= 0)      snprintf(buf, sizeof(buf), "PM2.5  %d \u00B5g/m\u00B3", a.pm25);
  else if (a.pm10 >= 0) snprintf(buf, sizeof(buf), "PM10  %d \u00B5g/m\u00B3", a.pm10);
  else                  snprintf(buf, sizeof(buf), "PM2.5  --");
  textCenter(F_BODY, COL_TEXT, HW_W / 2, 172, buf);

  if (a.observedAt[0]) {
    snprintf(buf, sizeof(buf), "Observed %s", a.observedAt);
    textCenter(F_SMALL, COL_MUTED, HW_W / 2, 192, buf);
  }
  if (a.stale) textCenter(F_SMALL, COL_MUTED, HW_W / 2, 206, "STALE");
}

static void wcMoon(const MoonData& m) {
  statusBar("Moon");

  int illum = m.valid ? m.illumPct : -1;
  bool waxing = m.valid ? m.waxing : true;
  drawMoon(HW_W / 2, 88, 28, illum, waxing, COL_MOON);

  if (m.valid && m.phaseName[0]) {
    textCenter(F_HEAD, COL_TEXT, HW_W / 2, 146, m.phaseName);
    char buf[24];
    snprintf(buf, sizeof(buf), "Illumination %d%%", m.illumPct);
    textCenter(F_BODY, COL_DIM, HW_W / 2, 170, buf);
  } else {
    textCenter(F_HEAD, COL_MUTED, HW_W / 2, 150, "no moon data");
  }
}

static void wcMoonset(const MoonData& m) {
  statusBar("Moonrise");

  char t[8];
  int y = 92;

  textAt(F_BODY, COL_DIM, SAFE_L, y, "Moonrise");
  fmtClock(t, sizeof(t), m.valid ? m.moonriseMin : -1);
  textRight(F_MED, COL_MOON, WC_R, y, t);

  y += 36;
  textAt(F_BODY, COL_DIM, SAFE_L, y, "Moonset");
  fmtClock(t, sizeof(t), m.valid ? m.moonsetMin : -1);
  textRight(F_MED, COL_DIM, WC_R, y, t);

  spr.drawFastHLine(X(SAFE_L), Y(152), WC_R - SAFE_L, COL_LINE);
  if (m.valid && m.phaseName[0]) {
    textCenter(F_BODY, COL_DIM, HW_W / 2, 178, m.phaseName);
  }
}

void uiDrawWeatherCard(int card, const WeatherData& w, const MoonData& m,
                       const AirData& a, bool hasAirToken, int ox, int oy) {
  g_ox = ox; g_oy = oy;
  cardBg();
  switch (card) {
    case UI_WC_SUN:     wcSun(w); break;
    case UI_WC_AIR:     wcAir(a, hasAirToken); break;
    case UI_WC_MOON:    wcMoon(m); break;
    case UI_WC_MOONSET: wcMoonset(m); break;
    default:            wcNow(w); break;
  }
}

// ---------------------------------------------------------------------------
// CLOCK card
// ---------------------------------------------------------------------------

static const char* WEEKDAYS[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

void uiDrawClock(int ox, int oy) {
  g_ox = ox; g_oy = oy;
  cardBg();
  statusBar(g_env.timeValid ? "Synced" : "Clock");

  char buf[32];

  if (!g_env.timeValid) {
    textCenter(F_HEAD, COL_MUTED, HW_W / 2, 110, "syncing time...");
  } else {
    snprintf(buf, sizeof(buf), "%02d:%02d", g_env.hh, g_env.mm);
    textCenter(F_BIG, COL_TEXT, HW_W / 2, 92, buf);

    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %s", g_env.year, g_env.month, g_env.day,
             WEEKDAYS[(g_env.weekday < 0 || g_env.weekday > 6) ? 0 : g_env.weekday]);
    textCenter(F_BODY, COL_DIM, HW_W / 2, 118, buf);
  }

  spr.drawFastHLine(X(SAFE_L), Y(134), SAFE_W, COL_LINE);

  uint16_t bcol = g_env.batteryPct <= 20 ? COL_RED
                : g_env.batteryPct <= 40 ? COL_YELLOW : COL_GREEN;
  const char* bstate = g_env.charging ? "charging" : (g_env.usbPresent ? "full" : "");
  if (bstate[0]) snprintf(buf, sizeof(buf), "Battery %d%%  %s", g_env.batteryPct, bstate);
  else           snprintf(buf, sizeof(buf), "Battery %d%%", g_env.batteryPct);
  textAt(F_BODY, COL_DIM, SAFE_L, 156, buf);

  roundBar(SAFE_L, 160, SAFE_W, 12, g_env.batteryPct, bcol);
}

// ---------------------------------------------------------------------------
// Agent-activity animations
//
// Both "Thinking..." (running) and "Needs you" (waiting) are a soft breathing
// ring: a dim halo plus a brighter core outline whose radius and brightness
// rise and fall on a sine, so the panel glows like a breathing light instead of
// strobing. There is no bitmap here any more — the old 555 clip-art is gone.
// ---------------------------------------------------------------------------

// Scale an RGB565 colour towards black (0..1). The background is black, so this
// is a pure brightness ramp — enough for a glowing ring with no alpha blend.
static uint16_t dim565(uint16_t c, float f) {
  if (f < 0.0f) f = 0.0f;
  if (f > 1.0f) f = 1.0f;
  int r = (int)(((c >> 11) & 0x1F) * f + 0.5f);
  int g = (int)(((c >>  5) & 0x3F) * f + 0.5f);
  int b = (int)(( c        & 0x1F) * f + 0.5f);
  return (uint16_t)((r << 11) | (g << 5) | b);
}

// One full breath over `periodMs`, in [0,1]; smooth at both ends (no strobe).
static float breathe01(uint32_t elapsedMs, uint32_t periodMs) {
  float phase = (float)(elapsedMs % periodMs) / (float)periodMs;
  return 0.5f - 0.5f * cosf(2.0f * (float)M_PI * phase);
}

// A breathing ring at logical centre (cx, cy): three dim halo outlines plus a
// brighter core, all growing and brightening together. The core never reaches
// black, so the ring glows dimly at the trough of the breath rather than
// blinking out.
static void drawBreathingRing(int cx, int cy, int baseR, uint16_t col, float b) {
  int r = baseR + (int)(5.0f * b);
  for (int i = 3; i >= 1; i--) {
    spr.drawCircle(cx, cy, r + i * 3, dim565(col, (0.10f + 0.35f * b) / (float)i));
  }
  spr.drawCircle(cx, cy, r, dim565(col, 0.25f + 0.75f * b));
}

void usageDisplayDrawThinking(uint32_t elapsedMs) {
  g_ox = 0; g_oy = 0;
  spr.fillScreen(COL_BG);
  drawBreathingRing(HW_W / 2, SAFE_T + 66, 30, COL_THINK,
                    breathe01(elapsedMs, 2200));
  textCenter(F_HEAD, COL_TEXT, HW_W / 2, SAFE_T + 150, "Thinking...");
}

// Agent is blocked on a permission / question. Same breathing ring, but amber
// and a touch faster and stronger, with a bang in the middle, so a glance still
// says "it needs you" rather than "it is working".
void usageDisplayDrawWaiting(uint32_t elapsedMs) {
  g_ox = 0; g_oy = 0;
  spr.fillScreen(COL_BG);
  float b = breathe01(elapsedMs, 1300);
  drawBreathingRing(HW_W / 2, SAFE_T + 72, 34, COL_ORANGE, b);
  textCenter(F_BIG, dim565(COL_YELLOW, 0.65f + 0.35f * b), HW_W / 2, SAFE_T + 92, "!");
  textCenter(F_HEAD, COL_TEXT, HW_W / 2, SAFE_T + 168, "Needs you");
}

// Firmware update in flight. Painted from the OTA progress callback (loop() is
// blocked inside the receive loop), and never dismissed: a successful push
// reboots straight from here. Deliberately text-only + one bar, so a redraw
// between flash writes stays cheap.
void usageDisplayDrawOta(uint32_t pct) {
  spr.fillScreen(COL_BG);
  textCenter(F_HEAD, COL_TEXT, HW_W / 2, SAFE_T + 56, "Updating");

  char buf[8];
  snprintf(buf, sizeof(buf), "%u%%", (unsigned)pct);
  textCenter(F_BIG, COL_GREEN, HW_W / 2, SAFE_T + 112, buf);

  roundBar(SAFE_L, SAFE_T + 140, SAFE_W, 14, (int)pct, COL_GREEN);
  textCenter(F_SMALL, COL_MUTED, HW_W / 2, SAFE_T + 186, "keep power on");
}
