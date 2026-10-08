#include "usage_display.h"
#include "hw/display.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <U8g2lib.h>
#include <LittleFS.h>

#define spr (*hwCanvas())

// ---------------------------------------------------------------------------
// Palette / fonts
// ---------------------------------------------------------------------------

static const uint16_t COL_BG     = 0x0000;
static const uint16_t COL_TEXT   = 0xFFFF;
static const uint16_t COL_DIM    = 0x9CF3;
static const uint16_t COL_MUTED  = 0x528A;
static const uint16_t COL_LINE   = 0x3186;
static const uint16_t COL_TRACK  = 0x2104;
static const uint16_t COL_GREEN  = 0x07E0;
static const uint16_t COL_YELLOW = 0xFFE0;
static const uint16_t COL_ORANGE = 0xFD20;
static const uint16_t COL_RED    = 0xF800;
static const uint16_t COL_PURPLE = 0xC81F;
static const uint16_t COL_MAROON = 0x8000;
static const uint16_t COL_SUN    = 0xFFE0;
static const uint16_t COL_CLOUD  = 0xC618;
static const uint16_t COL_RAIN   = 0x5D9F;
static const uint16_t COL_SNOW   = 0xE73F;
static const uint16_t COL_MOON   = 0xEF7D;
static const uint16_t COL_MOONDK = 0x39E7;

static const uint8_t* F_BIG   = u8g2_font_logisoso28_tf;
static const uint8_t* F_MED   = u8g2_font_logisoso22_tf;
static const uint8_t* F_HEAD  = u8g2_font_helvB12_tf;
static const uint8_t* F_BODY  = u8g2_font_helvB10_tf;
static const uint8_t* F_SMALL = u8g2_font_helvB08_tf;

static UiEnv g_env;

// Weather cards keep this right margin free for the vertical card dots.
static const int WC_R = SAFE_R - 12;

// Forward declaration (defined further down with the 555 artwork).
static void drawLogo555(int cx, int cy);

// Tile origin for the current card (set by each uiDraw* entry point).
static int g_ox = 0, g_oy = 0;
#define X(v) ((v) + g_ox)
#define Y(v) ((v) + g_oy)

// ---------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------

static void textAt(const uint8_t* f, uint16_t color, int x, int baseline, const char* s) {
  spr.setFont(f);
  spr.setTextColor(color);
  spr.setCursor(X(x), Y(baseline));
  spr.print(s);
}

static int textW(const uint8_t* f, const char* s) {
  spr.setFont(f);
  int16_t x1, y1;
  uint16_t w, h;
  spr.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  return (int)w;
}

static void textRight(const uint8_t* f, uint16_t color, int xRight, int baseline, const char* s) {
  spr.setFont(f);
  spr.setTextColor(color);
  int w = textW(f, s);
  spr.setCursor(X(xRight) - w, Y(baseline));
  spr.print(s);
}

static void textCenter(const uint8_t* f, uint16_t color, int cx, int baseline, const char* s) {
  spr.setFont(f);
  spr.setTextColor(color);
  int w = textW(f, s);
  spr.setCursor(X(cx) - w / 2, Y(baseline));
  spr.print(s);
}

// ---------------------------------------------------------------------------
// Primitive drawing
// ---------------------------------------------------------------------------

static int isqrt(int v) {
  if (v <= 0) return 0;
  int x = (int)sqrtf((float)v);
  while ((long)(x + 1) * (x + 1) <= v) x++;
  while ((long)x * x > v) x--;
  return x;
}

static void roundBar(int x, int y, int w, int h, int pct, uint16_t fill) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  spr.fillRoundRect(X(x), Y(y), w, h, h / 2, COL_TRACK);
  int fw = (w - 2) * pct / 100;
  if (fw <= 0) return;
  if (fw < h) fw = h;                       // keep the pill cap intact
  spr.fillRoundRect(X(x) + 1, Y(y) + 1, fw, h - 2, (h - 2) / 2, fill);
}

static void drawSun(int cx, int cy, int r, uint16_t c) {
  spr.fillCircle(X(cx), Y(cy), r, c);
  for (int i = 0; i < 8; i++) {
    float a = i * (float)M_PI / 4.0f;
    int x1 = cx + (int)((r + 2) * cosf(a));
    int y1 = cy + (int)((r + 2) * sinf(a));
    int x2 = cx + (int)((r + 7) * cosf(a));
    int y2 = cy + (int)((r + 7) * sinf(a));
    spr.drawLine(X(x1), Y(y1), X(x2), Y(y2), c);
  }
}

static void drawCloud(int cx, int cy, int w, uint16_t c) {
  int r = w / 6;
  if (r < 3) r = 3;
  spr.fillCircle(X(cx - r * 2), Y(cy), r, c);
  spr.fillCircle(X(cx + r * 2), Y(cy - r / 3), r, c);
  spr.fillCircle(X(cx), Y(cy - r), r + 1, c);
  spr.fillRect(X(cx - r * 2), Y(cy), r * 4, r + 1, c);
}

static void drawRain(int cx, int cy, int n, uint16_t c) {
  for (int i = 0; i < n; i++) {
    int x = cx - (n - 1) * 6 + i * 12;
    spr.drawLine(X(x), Y(cy), X(x - 4), Y(cy + 10), c);
  }
}

static void drawSnow(int cx, int cy, int n, uint16_t c) {
  for (int i = 0; i < n; i++) {
    int x = cx - (n - 1) * 7 + i * 14;
    int y = cy + 5;
    spr.drawLine(X(x - 3), Y(y), X(x + 3), Y(y), c);
    spr.drawLine(X(x), Y(y - 3), X(x), Y(y + 3), c);
    spr.drawLine(X(x - 2), Y(y - 2), X(x + 2), Y(y + 2), c);
    spr.drawLine(X(x - 2), Y(y + 2), X(x + 2), Y(y - 2), c);
  }
}

static void drawBolt(int cx, int cy, uint16_t c) {
  spr.fillTriangle(X(cx - 1), Y(cy), X(cx + 6), Y(cy - 8), X(cx + 3), Y(cy + 1), c);
  spr.fillTriangle(X(cx + 2), Y(cy + 1), X(cx - 5), Y(cy + 10), X(cx), Y(cy + 1), c);
}

static void drawFog(int cx, int cy, uint16_t c) {
  spr.fillRoundRect(X(cx - 16), Y(cy - 8), 32, 3, 2, c);
  spr.fillRoundRect(X(cx - 20), Y(cy), 40, 3, 2, c);
  spr.fillRoundRect(X(cx - 14), Y(cy + 8), 28, 3, 2, c);
}

// kind: 0 clear, 1 partly, 2 cloudy, 3 overcast, 4 fog, 5 thunder,
//       6 rain, 7 snow, 8 sleet
static int kindFromText(const char* c) {
  if (!c) return 2;
  if (strstr(c, "Thunder")) return 5;
  if (strstr(c, "Blizzard")) return 7;
  if (strstr(c, "Snow") || strstr(c, "snow")) return 7;
  if (strstr(c, "Sleet") || strstr(c, "Ice") || strstr(c, "Freezing")) return 8;
  if (strstr(c, "Rain") || strstr(c, "Drizzle") || strstr(c, "rain")) return 6;
  if (strstr(c, "Fog") || strstr(c, "Mist")) return 4;
  if (strstr(c, "Haze") || strstr(c, "Smoky")) return 4;
  if (strstr(c, "Overcast")) return 3;
  if (strstr(c, "Cloudy")) return 2;
  if (strstr(c, "Partly")) return 1;
  if (strstr(c, "Sunny") || strstr(c, "Clear")) return 0;
  return 2;
}

static void drawWeatherIcon(int kind, int cx, int cy) {
  switch (kind) {
    case 0:
      drawSun(cx, cy, 13, COL_SUN);
      break;
    case 1:
      drawSun(cx + 9, cy - 7, 7, COL_SUN);
      drawCloud(cx - 5, cy + 6, 30, COL_CLOUD);
      break;
    case 2:
      drawCloud(cx, cy, 36, COL_CLOUD);
      break;
    case 3:
      drawCloud(cx - 8, cy - 4, 28, COL_DIM);
      drawCloud(cx + 6, cy + 5, 30, COL_CLOUD);
      break;
    case 4:
      drawFog(cx, cy, COL_CLOUD);
      break;
    case 5:
      drawCloud(cx, cy - 6, 34, COL_CLOUD);
      drawBolt(cx, cy + 12, COL_SUN);
      break;
    case 6:
      drawCloud(cx, cy - 6, 34, COL_CLOUD);
      drawRain(cx, cy + 10, 3, COL_RAIN);
      break;
    case 7:
      drawCloud(cx, cy - 6, 34, COL_CLOUD);
      drawSnow(cx, cy + 10, 2, COL_SNOW);
      break;
    default:
      drawCloud(cx, cy - 6, 34, COL_CLOUD);
      drawRain(cx, cy + 10, 2, COL_RAIN);
      drawSnow(cx + 6, cy + 10, 1, COL_SNOW);
      break;
  }
}

// 16-point compass -> 8-way arrow, drawn with lines.
static void drawWindArrow(int x, int y, const char* dir, uint16_t c) {
  static const char* D8[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
  int idx = 0;
  for (int i = 0; i < 8; i++) {
    if (strstr(dir, D8[i])) { idx = i; break; }
  }
  // Map compass degrees: N=0 at top, clockwise.
  float a = (idx * 45.0f - 90.0f) * (float)M_PI / 180.0f;
  int x1 = x - (int)(7 * cosf(a));
  int y1 = y - (int)(7 * sinf(a));
  int x2 = x + (int)(7 * cosf(a));
  int y2 = y + (int)(7 * sinf(a));
  spr.drawLine(X(x1), Y(y1), X(x2), Y(y2), c);
  spr.drawLine(X(x2), Y(y2), X(x2 - (int)(4 * cosf(a - 0.6f))), Y(y2 - (int)(4 * sinf(a - 0.6f))), c);
  spr.drawLine(X(x2), Y(y2), X(x2 - (int)(4 * cosf(a + 0.6f))), Y(y2 - (int)(4 * sinf(a + 0.6f))), c);
}

// Row-scanned moon: exact terminator shape from the illumination percentage.
static void drawMoon(int cx, int cy, int r, int illumPct, bool waxing, uint16_t lit) {
  spr.fillCircle(X(cx), Y(cy), r, COL_MOONDK);
  if (illumPct < 0) illumPct = 0;
  if (illumPct > 100) illumPct = 100;
  int k = (int)((1.0f - 2.0f * (illumPct / 100.0f)) * 256.0f + 0.5f);
  for (int dy = -r; dy <= r; dy++) {
    int w = isqrt(r * r - dy * dy);
    int xt = (k * w) / 256;
    int x0 = waxing ? xt : -w;
    int x1 = waxing ? w : -xt;
    if (x1 < x0) { int t = x0; x0 = x1; x1 = t; }
    if (x1 >= x0) spr.drawFastHLine(X(cx + x0), Y(cy + dy), x1 - x0 + 1, lit);
  }
  spr.drawCircle(X(cx), Y(cy), r, COL_MOON);
}

static uint16_t usageColor(int pct) {
  if (pct > 60) return COL_RED;
  if (pct > 30) return COL_YELLOW;
  return COL_GREEN;
}

static const char* aqiCategory(int aqi) {
  if (aqi < 0) return "--";
  if (aqi <= 50) return "Good";
  if (aqi <= 100) return "Moderate";
  if (aqi <= 150) return "Sensitive";
  if (aqi <= 200) return "Unhealthy";
  if (aqi <= 300) return "Very Unhealthy";
  return "Hazardous";
}

static uint16_t aqiColor(int aqi) {
  if (aqi < 0) return COL_MUTED;
  if (aqi <= 50) return COL_GREEN;
  if (aqi <= 100) return COL_YELLOW;
  if (aqi <= 150) return COL_ORANGE;
  if (aqi <= 200) return COL_RED;
  if (aqi <= 300) return COL_PURPLE;
  return COL_MAROON;
}

// ---------------------------------------------------------------------------
// Chrome: status bar, divider, page dots
// ---------------------------------------------------------------------------

static void cardBg() {
  spr.fillRect(g_ox, g_oy, HW_W, HW_H, COL_BG);
}

static void statusBar(const char* title) {
  spr.fillCircle(X(SAFE_L + 3), Y(20), 3, g_env.online ? COL_GREEN : COL_RED);
  textAt(F_HEAD, COL_TEXT, SAFE_L + 13, 25, title);

  char clk[8];
  if (g_env.timeValid) snprintf(clk, sizeof(clk), "%02d:%02d", g_env.hh, g_env.mm);
  else                 snprintf(clk, sizeof(clk), "--:--");
  textRight(F_HEAD, COL_DIM, SAFE_R, 25, clk);

  spr.drawFastHLine(X(SAFE_L), Y(33), SAFE_W, COL_LINE);
}

void uiDrawPageDots(int active) {
  int cx = HW_W / 2;
  int y = SAFE_B - 5;
  int n = UI_PAGE_COUNT;
  int span = 12;
  int x0 = cx - (n - 1) * span / 2;
  for (int i = 0; i < n; i++) {
    int x = x0 + i * span;
    int r = (i == active) ? 3 : 2;
    spr.fillCircle(x, y, r, (i == active) ? COL_TEXT : COL_MUTED);
  }
}

void uiDrawCardDots(int active, int count) {
  int x = SAFE_R - 6;
  int span = 12;
  int y0 = SAFE_T + SAFE_H / 2 - (count - 1) * span / 2;
  for (int i = 0; i < count; i++) {
    int y = y0 + i * span;
    int r = (i == active) ? 3 : 2;
    spr.fillCircle(x, y, r, (i == active) ? COL_TEXT : COL_MUTED);
  }
}

void uiBeginFrame() {
  spr.fillScreen(COL_BG);
}

void uiSetEnv(const UiEnv& env) { g_env = env; }

const char* uiPageTitle(int page) {
  switch (page) {
    case UI_PAGE_USAGE: return "Usage";
    case UI_PAGE_CLOCK: return "Clock";
    default:            return (g_env.city && g_env.city[0]) ? g_env.city : "Weather";
  }
}

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
  statusBar(g_env.city && g_env.city[0] ? g_env.city : "Beijing");

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

  textCenter(F_SMALL, COL_MUTED, HW_W / 2, 54, "US Embassy Beijing");

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

  drawLogo555(HW_W / 2, 188);
}

void uiDrawAuthNeeded(int ox, int oy) {
  g_ox = ox; g_oy = oy;
  cardBg();
  statusBar("Setup");

  textCenter(F_HEAD, COL_YELLOW, HW_W / 2, 84, "AUTH NEEDED");
  textCenter(F_BODY, COL_TEXT, HW_W / 2, 116, "run tools/authorize.py");
  textCenter(F_SMALL, COL_MUTED, HW_W / 2, 140, "then uploadfs");
}

// ---------------------------------------------------------------------------
// 555 easter-egg artwork
// ---------------------------------------------------------------------------

static uint16_t* s_img555 = nullptr;
static int s_img555W = 0, s_img555H = 0;

static bool loadImg555() {
  if (s_img555) return true;
  File f = LittleFS.open("/555.raw", "r");
  if (!f) return false;
  size_t sz = f.size();
  s_img555 = (uint16_t*)malloc(sz);
  if (!s_img555) { f.close(); return false; }
  f.read((uint8_t*)s_img555, sz);
  f.close();
  s_img555W = 160;
  s_img555H = 26;
  return true;
}

static void drawLogo555(int cx, int cy) {
  if (!loadImg555()) return;
  spr.draw16bitRGBBitmap(cx - s_img555W / 2, cy - s_img555H / 2, s_img555, s_img555W, s_img555H);
}

void usageDisplayDrawEaster555(uint32_t elapsedMs) {
  if (!loadImg555()) { spr.fillScreen(COL_BG); return; }
  spr.fillScreen(COL_BG);
  uint32_t cycle = elapsedMs % 200;
  if (cycle < 100) {
    spr.draw16bitRGBBitmap(HW_W / 2 - s_img555W / 2, HW_H / 2 - s_img555H / 2,
                           s_img555, s_img555W, s_img555H);
  }
}

void usageDisplayDrawThinking(uint32_t elapsedMs) {
  if (!loadImg555()) { spr.fillScreen(COL_BG); return; }
  spr.fillScreen(COL_BG);
  uint32_t cycle = elapsedMs % 200;
  if (cycle < 100) {
    spr.draw16bitRGBBitmap(HW_W / 2 - s_img555W / 2, SAFE_T + 40,
                           s_img555, s_img555W, s_img555H);
  }
  spr.setFont(u8g2_font_helvB12_tf);
  spr.setTextColor(COL_TEXT);
  spr.setCursor(HW_W / 2 - 26, SAFE_T + 150);
  spr.print("Thinking...");
}
