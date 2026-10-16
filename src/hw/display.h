// src/hw/display.h
#pragma once
#include <Arduino_GFX_Library.h>
#include <cstdarg>
#include <cstdio>
#include "hw/pins.h"   // provides BOARD_HW_W, BOARD_HW_H, BOARD_SAFE_INSET, BOARD_DISPLAY_UI_SCALE
#include "ui/lvgl_text.h"

// Logical drawing grid (what the UI code is authored against).
constexpr int HW_W       = BOARD_HW_W;
constexpr int HW_H       = BOARD_HW_H;

// Physical canvas raster size (logical × supersample factor). The actual
// Arduino_Canvas framebuffer is this size; ScaledCanvas maps logical UI
// coordinates onto it.
constexpr int UI_SCALE       = BOARD_DISPLAY_UI_SCALE;
constexpr int HW_CANVAS_W    = HW_W * UI_SCALE;
constexpr int HW_CANVAS_H    = HW_H * UI_SCALE;

// Logical-canvas safe-draw region for the rounded/circular AMOLED bezel.
constexpr int SAFE_INSET = BOARD_SAFE_INSET;
constexpr int SAFE_L     = SAFE_INSET;
constexpr int SAFE_T     = SAFE_INSET;
constexpr int SAFE_R     = HW_W - SAFE_INSET;
constexpr int SAFE_B     = HW_H - SAFE_INSET;
constexpr int SAFE_W     = HW_W - 2 * SAFE_INSET;
constexpr int SAFE_H     = HW_H - 2 * SAFE_INSET;

// ---------------------------------------------------------------------------
// ScaledCanvas — a thin view over the physical Arduino_Canvas that multiplies
// every coordinate/size by UI_SCALE, so all existing UI code can keep drawing
// in 184×224 logical units while the pixels are rasterised at 368×448.
// ---------------------------------------------------------------------------
class ScaledCanvas {
public:
  ScaledCanvas(Arduino_Canvas* c, uint8_t scale) : _c(c), _s(scale ? scale : 1) {}

  void fillScreen(uint16_t color) { _c->fillScreen(color); }

  // Text goes through LVGL (see ui/lvgl_text.h) so the whole UI — including the
  // WiFi setup screen — shares one 4bpp anti-aliased pipeline. setCursor() takes
  // a logical baseline, matching the old u8g2 behaviour.
  void setFont(const lv_font_t* f) { _font = f; }
  void setTextColor(uint16_t c) { _fg = c; }
  void setTextColor(uint16_t c, uint16_t /*bg*/) { _fg = c; }  // text is transparent

  void setCursor(int16_t x, int16_t y) { _cx = (int)x * _s; _cy = (int)y * _s; }

  void print(const char* s) {
    if (!_font || !s || !s[0]) return;
    lvglDrawText(_font, _fg, _cx, _cy, s);
    _cx += lvglTextWidth(_font, s);
  }
  void print(char c) { char b[2] = { c, 0 }; print(b); }

  void printf(const char* fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    print(buf);
  }

  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) {
    _c->fillRect((int16_t)(x * _s), (int16_t)(y * _s),
                 (int16_t)(w * _s), (int16_t)(h * _s), c);
  }
  void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) {
    _c->drawRect((int16_t)(x * _s), (int16_t)(y * _s),
                 (int16_t)(w * _s), (int16_t)(h * _s), c);
  }
  void fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t c) {
    _c->fillRoundRect((int16_t)(x * _s), (int16_t)(y * _s),
                      (int16_t)(w * _s), (int16_t)(h * _s), (int16_t)(r * _s), c);
  }
  void drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t c) {
    _c->drawRoundRect((int16_t)(x * _s), (int16_t)(y * _s),
                      (int16_t)(w * _s), (int16_t)(h * _s), (int16_t)(r * _s), c);
  }
  void fillCircle(int16_t x, int16_t y, int16_t r, uint16_t c) {
    _c->fillCircle((int16_t)(x * _s), (int16_t)(y * _s), (int16_t)(r * _s), c);
  }
  void drawCircle(int16_t x, int16_t y, int16_t r, uint16_t c) {
    _c->drawCircle((int16_t)(x * _s), (int16_t)(y * _s), (int16_t)(r * _s), c);
  }
  void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t c) {
    _c->drawLine((int16_t)(x0 * _s), (int16_t)(y0 * _s),
                 (int16_t)(x1 * _s), (int16_t)(y1 * _s), c);
  }
  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t c) {
    _c->drawFastHLine((int16_t)(x * _s), (int16_t)(y * _s), (int16_t)(w * _s), c);
  }
  void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t c) {
    _c->drawFastVLine((int16_t)(x * _s), (int16_t)(y * _s), (int16_t)(h * _s), c);
  }
  void fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                    int16_t x2, int16_t y2, uint16_t c) {
    _c->fillTriangle((int16_t)(x0 * _s), (int16_t)(y0 * _s),
                     (int16_t)(x1 * _s), (int16_t)(y1 * _s),
                     (int16_t)(x2 * _s), (int16_t)(y2 * _s), c);
  }

  uint16_t* getFramebuffer() { return _c->getFramebuffer(); }

private:
  Arduino_Canvas*  _c;
  uint8_t          _s;
  const lv_font_t* _font = nullptr;
  uint16_t         _fg   = 0xFFFF;   // current text colour (RGB565)
  int              _cx   = 0;        // text cursor, in canvas pixels
  int              _cy   = 0;
};

bool hwDisplayInit();
void hwDisplayPush();
void hwDisplayBrightness(uint8_t lvl_0_4);
ScaledCanvas* hwCanvas();

// Shorthand used by the UI translation units to draw on the logical canvas.
#define spr (*hwCanvas())

// Map a raw touch coordinate (physical panel pixels) to the logical UI grid
// (BOARD_HW_W × BOARD_HW_H) that all drawing is authored against. This is the
// exact inverse of hwDisplayPush()'s blit geometry, kept in one place so the
// display and touch paths can never drift apart.
void hwDisplayMapTouch(int px, int py, int* lx, int* ly);
