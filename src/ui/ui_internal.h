#pragma once
// Internal contract between the UI translation units (primitives / icons /
// cards). Not a public header — consumers include "usage_display.h" instead.
#include <stdint.h>
#include <U8g2lib.h>
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

// Fonts (selected per supersample factor in primitives.cpp).
extern const uint8_t* F_BIG;
extern const uint8_t* F_MED;
extern const uint8_t* F_HEAD;
extern const uint8_t* F_BODY;
extern const uint8_t* F_SMALL;

// primitives.cpp
void textAt(const uint8_t* f, uint16_t color, int x, int baseline, const char* s);
int  textW(const uint8_t* f, const char* s);
void textRight(const uint8_t* f, uint16_t color, int xRight, int baseline, const char* s);
void textCenter(const uint8_t* f, uint16_t color, int cx, int baseline, const char* s);
void roundBar(int x, int y, int w, int h, int pct, uint16_t fill);
void cardBg();
void statusBar(const char* title);

// icons.cpp
void drawWeatherIcon(int kind, int cx, int cy);
void drawWindArrow(int x, int y, const char* dir, uint16_t c);
void drawMoon(int cx, int cy, int r, int illumPct, bool waxing, uint16_t lit);
uint16_t usageColor(int pct);
uint16_t aqiColor(int aqi);
