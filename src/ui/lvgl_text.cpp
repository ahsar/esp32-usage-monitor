#include "ui/lvgl_text.h"
#include "hw/display.h"
#include <Arduino.h>
#include <esp_heap_caps.h>

static lv_obj_t* s_canvas = nullptr;

// LVGL will not create any object until a display is registered. We never
// render that display — the canvas draws synchronously via lv_canvas_draw_text —
// so a tiny dummy driver is all that is needed.
static void dummyFlush(lv_disp_drv_t* drv, const lv_area_t*, lv_color_t*) {
  lv_disp_flush_ready(drv);
}

static inline lv_color_t c16(uint16_t v) {
  lv_color_t c;
  c.full = v;   // LV_COLOR_16_SWAP == 0 → native RGB565, same as the canvas
  return c;
}

void lvglTextInit() {
  if (s_canvas) return;
  lv_init();

  static lv_color_t dummy[64];
  static lv_disp_draw_buf_t drawBuf;
  static lv_disp_drv_t drv;
  lv_disp_draw_buf_init(&drawBuf, dummy, nullptr, 64);
  lv_disp_drv_init(&drv);
  drv.draw_buf = &drawBuf;
  drv.flush_cb = dummyFlush;
  drv.hor_res  = 8;
  drv.ver_res  = 8;
  lv_disp_drv_register(&drv);

  uint16_t* fb = (uint16_t*)hwCanvas()->getFramebuffer();
  s_canvas = lv_canvas_create(lv_scr_act());
  lv_canvas_set_buffer(s_canvas, fb, HW_CANVAS_W, HW_CANVAS_H, LV_IMG_CF_TRUE_COLOR);
}

void lvglDrawText(const lv_font_t* f, uint16_t rgb565, int x, int baseline, const char* s,
                  bool nowrap) {
  if (!s_canvas || !f || !s || !s[0]) return;
  if (x >= HW_CANVAS_W) return;

  lv_draw_label_dsc_t dsc;
  lv_draw_label_dsc_init(&dsc);
  dsc.color = c16(rgb565);
  dsc.font  = f;
  dsc.opa   = LV_OPA_COVER;
  dsc.align = LV_TEXT_ALIGN_LEFT;
  // LV_TEXT_FLAG_EXPAND is "ignore max_width", i.e. do not wrap. See the header:
  // a wrapped line is drawn `line_height` lower, on top of the next line here.
  if (nowrap) dsc.flag |= LV_TEXT_FLAG_EXPAND;

  // lv_draw_label places the line-box top at y; our callers pass the baseline.
  int yTop = baseline - (int)(f->line_height - f->base_line);
  lv_canvas_draw_text(s_canvas, x, yTop, HW_CANVAS_W - x, &dsc, s);
}

int lvglTextWidth(const lv_font_t* f, const char* s) {
  if (!f || !s || !s[0]) return 0;
  lv_point_t size;
  lv_txt_get_size(&size, s, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  return (int)size.x;
}
