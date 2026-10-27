#pragma once
// Internal contract between the UI translation units (primitives / icons /
// cards). Not a public header — consumers include "usage_display.h" instead.
#include <stdint.h>
#include <lvgl.h>
#include "hw/display.h"
#include "ui_model.h"

// Shared per-frame environment (status bar + clock card). Owned by
// primitives.cpp, set via uiSetEnv().
extern UiEnv g_env;

// Tile origin for the current card, set by each uiDraw* entry point and used by
// the X()/Y() coordinate macros so a card can be rendered at any swipe offset.
extern int g_ox, g_oy;
#define X(v) ((v) + g_ox)
#define Y(v) ((v) + g_oy)

// Text fonts: LVGL Montserrat (4bpp anti-aliased), selected per supersample
// factor in primitives.cpp and rasterised via ui/lvgl_text.{h,cpp}.
extern const lv_font_t* F_BIG;
extern const lv_font_t* F_MED;
extern const lv_font_t* F_HEAD;
extern const lv_font_t* F_BODY;
extern const lv_font_t* F_SMALL;

// primitives.cpp
void textAt(const lv_font_t* f, uint16_t color, int x, int baseline, const char* s);
int  textW(const lv_font_t* f, const char* s);
void textRight(const lv_font_t* f, uint16_t color, int xRight, int baseline, const char* s);
void textCenter(const lv_font_t* f, uint16_t color, int cx, int baseline, const char* s);
void roundBar(int x, int y, int w, int h, int pct, uint16_t fill);
void cardBg();
void statusBar(const char* title);
// Title bar with a muted note after the title (the Usage page's data age).
// Clipped before the clock so it can never run into it.
void statusBar2(const char* title, const char* note, uint16_t noteColor);

// Animation clock (see uiSetAnimMs). 0 = static frame.
uint32_t uiAnimMs();

// icons.cpp
void drawWeatherIcon(int kind, int cx, int cy);
// The little bolt next to the Clock page's "charging" word. Its own glyph rather
// than the weather one: that one is 18 logical px tall and sized for a sky icon,
// this one has to sit on the battery row's baseline without pushing the text.
void drawChargeBolt(int cx, int cy, uint16_t c);
void drawWindArrow(int x, int y, const char* dir, uint16_t c);
void drawMoon(int cx, int cy, int r, int illumPct, bool waxing, uint16_t lit);
uint16_t usageColor(int pct);
uint16_t aqiColor(int aqi);
