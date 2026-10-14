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
void lvglDrawText(const lv_font_t* f, uint16_t rgb565, int x, int baseline, const char* s);

// Pixel width of `s` in `f` (single line, no wrapping).
int lvglTextWidth(const lv_font_t* f, const char* s);
