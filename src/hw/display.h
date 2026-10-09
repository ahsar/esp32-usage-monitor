// src/hw/display.h
#pragma once
#include <Arduino_GFX_Library.h>
#include <cstdarg>
#include <cstdio>
#include "hw/pins.h"   // provides BOARD_HW_W, BOARD_HW_H, BOARD_SAFE_INSET, BOARD_DISPLAY_UI_SCALE

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
// Bitmaps are expected to already be pre-scaled by UI_SCALE (only used by the
// 555 easter-egg artwork), so their width/height are multiplied too.
// ---------------------------------------------------------------------------
class ScaledCanvas {
public:
  ScaledCanvas(Arduino_Canvas* c, uint8_t scale) : _c(c), _s(scale ? scale : 1) {}

  void fillScreen(uint16_t color) { _c->fillScreen(color); }

  void setFont(const uint8_t* f) { _c->setFont(f); }
  void setTextColor(uint16_t c) { _c->setTextColor(c); }
  void setTextColor(uint16_t c, uint16_t bg) { _c->setTextColor(c, bg); }

  void setCursor(int16_t x, int16_t y) { _c->setCursor((int16_t)(x * _s), (int16_t)(y * _s)); }
  void print(const char* s) { _c->print(s); }
  void print(char c) { _c->print(c); }

  void printf(const char* fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    _c->print(buf);
  }

  void getTextBounds(const char* s, int16_t x, int16_t y,
                     int16_t* x1, int16_t* y1, uint16_t* w, uint16_t* h) {
    _c->getTextBounds(s, (int16_t)(x * _s), (int16_t)(y * _s), x1, y1, w, h);
    if (x1) *x1 = (int16_t)(*x1 / _s);
    if (y1) *y1 = (int16_t)(*y1 / _s);
    if (w)  *w  = (uint16_t)(*w / _s);
    if (h)  *h  = (uint16_t)(*h / _s);
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
  void draw16bitRGBBitmap(int16_t x, int16_t y, uint16_t* bmp, int16_t w, int16_t h) {
    _c->draw16bitRGBBitmap((int16_t)(x * _s), (int16_t)(y * _s),
                           bmp, (int16_t)(w * _s), (int16_t)(h * _s));
  }

  uint16_t* getFramebuffer() { return _c->getFramebuffer(); }

private:
  Arduino_Canvas* _c;
  uint8_t         _s;
};

bool hwDisplayInit();
void hwDisplayPush();
void hwDisplayBrightness(uint8_t lvl_0_4);
ScaledCanvas* hwCanvas();

// Map a raw touch coordinate (physical panel pixels) to the logical UI grid
// (BOARD_HW_W × BOARD_HW_H) that all drawing is authored against. This is the
// exact inverse of hwDisplayPush()'s blit geometry, kept in one place so the
// display and touch paths can never drift apart.
void hwDisplayMapTouch(int px, int py, int* lx, int* ly);
