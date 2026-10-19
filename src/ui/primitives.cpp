// UI primitives: fonts, text helpers, bars, chrome (status bar + dots).
#include "ui/ui_internal.h"
#include "ui/lvgl_text.h"
#include "usage_display.h"
#include "ui/theme.h"
#include <cstdio>
#include <cstring>

using namespace ui_theme;

// ---------------------------------------------------------------------------
// Fonts
// ---------------------------------------------------------------------------

#if BOARD_DISPLAY_UI_SCALE > 1
// Supersampled canvas (368×448): Montserrat at roughly 2× the logical sizes.
// These are 4bpp anti-aliased, so glyph edges are smooth instead of the jagged
// 1-bit u8g2 ones. Regenerated locally to include µ/³ (see include/lv_conf.h).
const lv_font_t* F_BIG   = &lv_font_ui_56;
const lv_font_t* F_MED   = &lv_font_ui_40;
const lv_font_t* F_HEAD  = &lv_font_ui_24;
const lv_font_t* F_BODY  = &lv_font_ui_18;
const lv_font_t* F_SMALL = &lv_font_ui_14;
#else
// Native 184×224 canvas (no supersampling).
const lv_font_t* F_BIG   = &lv_font_ui_28;
const lv_font_t* F_MED   = &lv_font_ui_22;
const lv_font_t* F_HEAD  = &lv_font_ui_12;
const lv_font_t* F_BODY  = &lv_font_ui_10;
const lv_font_t* F_SMALL = &lv_font_ui_8;
#endif

int g_ox = 0, g_oy = 0;

UiEnv g_env;

// ---------------------------------------------------------------------------
// Text helpers (rasterised by LVGL into the canvas)
//
// The UI is authored on the logical grid but the canvas is UI_SCALE× bigger
// (and the LVGL fonts are already sized for the canvas), so positions must be
// scaled to canvas pixels here — textW()/textRight()/textCenter() keep working
// in logical units so callers and the icon code stay unchanged.
// ---------------------------------------------------------------------------

static inline int cx(int logicalX) { return X(logicalX) * UI_SCALE; }
static inline int cy(int logicalY) { return Y(logicalY) * UI_SCALE; }

void textAt(const lv_font_t* f, uint16_t color, int x, int baseline, const char* s) {
  lvglDrawText(f, color, cx(x), cy(baseline), s);
}

int textW(const lv_font_t* f, const char* s) {
  return lvglTextWidth(f, s) / UI_SCALE;   // logical px
}

void textRight(const lv_font_t* f, uint16_t color, int xRight, int baseline, const char* s) {
  lvglDrawText(f, color, cx(xRight) - lvglTextWidth(f, s), cy(baseline), s);
}

void textCenter(const lv_font_t* f, uint16_t color, int cxLogical, int baseline, const char* s) {
  lvglDrawText(f, color, cx(cxLogical) - lvglTextWidth(f, s) / 2, cy(baseline), s);
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
  // The WiFi setup screen turns wrapping off for its hand-placed lines (see
  // wifiSetupDraw); every main-UI frame starts from the default again.
  spr.setNoWrap(false);
}

void uiSetEnv(const UiEnv& env) { g_env = env; }
