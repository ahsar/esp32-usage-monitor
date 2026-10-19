#pragma once
// LVGL is used ONLY as an offscreen anti-aliased text rasteriser: an lv_canvas
// points at the existing Arduino_Canvas framebuffer and lv_canvas_draw_text()
// blends Montserrat 4bpp glyphs straight into it. No LVGL widgets/screens.
#include <stdint.h>
#include <lvgl.h>

// One-time bring-up (lv_init + a dummy display + the canvas over the
// framebuffer). Call after hwInit(), before the first render.
void lvglTextInit();

// Draw `s` with its baseline at (x, baseline), in canvas pixel coordinates.
//
// `nowrap` keeps it on one line: the text is laid out with LV_TEXT_FLAG_EXPAND
// and clipped at the canvas edge instead of being broken onto a second line
// `line_height` below. That second line lands on whatever is drawn next — the
// UI is fixed-position, so nothing expects a line to appear under another (the
// WiFi setup screen's `Tap to choose another network`, at 357 px in a 352 px
// area, drew its last word over the line below). Default stays wrapping so this
// is opt-in per draw.
void lvglDrawText(const lv_font_t* f, uint16_t rgb565, int x, int baseline, const char* s,
                  bool nowrap = false);

// Pixel width of `s` in `f` (single line, no wrapping).
int lvglTextWidth(const lv_font_t* f, const char* s);
