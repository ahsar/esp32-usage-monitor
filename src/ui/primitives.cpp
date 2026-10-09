// UI primitives: fonts, text helpers, bars, chrome (status bar + dots).
#include "ui/ui_internal.h"
#include "usage_display.h"
#include "ui/theme.h"
#include <cstdio>
#include <cstring>

using namespace ui_theme;

// ---------------------------------------------------------------------------
// Fonts
// ---------------------------------------------------------------------------

#if BOARD_DISPLAY_UI_SCALE > 1
// Supersampled canvas (see ScaledCanvas): the logical layout is scaled ×2, so
// fonts are bumped to ~2× size. Rendered natively at this size they stay crisp
// on the 314-PPI panel instead of being block-upscaled from a 184×224 raster.
const uint8_t* F_BIG   = u8g2_font_logisoso54_tf;
const uint8_t* F_MED   = u8g2_font_logisoso42_tf;
const uint8_t* F_HEAD  = u8g2_font_helvB24_tf;
const uint8_t* F_BODY  = u8g2_font_helvB18_tf;
const uint8_t* F_SMALL = u8g2_font_helvB14_tf;
#else
const uint8_t* F_BIG   = u8g2_font_logisoso28_tf;
const uint8_t* F_MED   = u8g2_font_logisoso22_tf;
const uint8_t* F_HEAD  = u8g2_font_helvB12_tf;
const uint8_t* F_BODY  = u8g2_font_helvB10_tf;
const uint8_t* F_SMALL = u8g2_font_helvB08_tf;
#endif

int g_ox = 0, g_oy = 0;

UiEnv g_env;

// ---------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------

void textAt(const uint8_t* f, uint16_t color, int x, int baseline, const char* s) {
  spr.setFont(f);
  spr.setTextColor(color);
  spr.setCursor(X(x), Y(baseline));
  spr.print(s);
}

int textW(const uint8_t* f, const char* s) {
  spr.setFont(f);
  int16_t x1, y1;
  uint16_t w, h;
  spr.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  return (int)w;
}

void textRight(const uint8_t* f, uint16_t color, int xRight, int baseline, const char* s) {
  spr.setFont(f);
  spr.setTextColor(color);
  int w = textW(f, s);
  spr.setCursor(X(xRight) - w, Y(baseline));
  spr.print(s);
}

void textCenter(const uint8_t* f, uint16_t color, int cx, int baseline, const char* s) {
  spr.setFont(f);
  spr.setTextColor(color);
  int w = textW(f, s);
  spr.setCursor(X(cx) - w / 2, Y(baseline));
  spr.print(s);
}

// ---------------------------------------------------------------------------
// Primitive drawing
// ---------------------------------------------------------------------------

void roundBar(int x, int y, int w, int h, int pct, uint16_t fill) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  spr.fillRoundRect(X(x), Y(y), w, h, h / 2, COL_TRACK);
  int fw = (w - 2) * pct / 100;
  if (fw <= 0) return;
  if (fw < h) fw = h;                       // keep the pill cap intact
  spr.fillRoundRect(X(x) + 1, Y(y) + 1, fw, h - 2, (h - 2) / 2, fill);
}

// ---------------------------------------------------------------------------
// Chrome: status bar, divider, page dots
// ---------------------------------------------------------------------------

void cardBg() {
  spr.fillRect(g_ox, g_oy, HW_W, HW_H, COL_BG);
}

void statusBar(const char* title) {
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
