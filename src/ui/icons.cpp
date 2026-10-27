// UI icons: weather glyphs, wind arrow, moon, and value -> colour mapping.
#include "ui/ui_internal.h"
#include "ui/theme.h"
#include <cmath>
#include <cstring>

using namespace ui_theme;

static int isqrt(int v) {
  if (v <= 0) return 0;
  int x = (int)sqrtf((float)v);
  while ((long)(x + 1) * (x + 1) <= v) x++;
  while ((long)x * x > v) x--;
  return x;
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

// The battery row's bolt: the same two-triangle shape as drawBolt(), scaled to
// ~10 logical px so it sits on a text baseline. The weather glyph is 18 px tall
// and offset for a sky icon — reusing it would push the whole row around.
void drawChargeBolt(int cx, int cy, uint16_t c) {
  spr.fillTriangle(X(cx - 1), Y(cy), X(cx + 4), Y(cy - 5), X(cx + 1), Y(cy), c);
  spr.fillTriangle(X(cx + 1), Y(cy), X(cx - 4), Y(cy + 5), X(cx - 1), Y(cy), c);
}

void drawWeatherIcon(int kind, int cx, int cy) {
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
void drawWindArrow(int x, int y, const char* dir, uint16_t c) {
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
void drawMoon(int cx, int cy, int r, int illumPct, bool waxing, uint16_t lit) {
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

uint16_t usageColor(int pct) {
  if (pct > 60) return COL_RED;
  if (pct > 30) return COL_YELLOW;
  return COL_GREEN;
}

uint16_t aqiColor(int aqi) {
  if (aqi < 0) return COL_MUTED;
  if (aqi <= 50) return COL_GREEN;
  if (aqi <= 100) return COL_YELLOW;
  if (aqi <= 150) return COL_ORANGE;
  if (aqi <= 200) return COL_RED;
  if (aqi <= 300) return COL_PURPLE;
  return COL_MAROON;
}
