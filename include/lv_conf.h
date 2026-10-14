#ifndef LV_CONF_H
#define LV_CONF_H
// Minimal LVGL config: we use LVGL ONLY as an offscreen anti-aliased text
// rasteriser (an lv_canvas over the existing Arduino_Canvas framebuffer plus
// the Montserrat 4bpp fonts). No LVGL widgets, screens or driven display.
#include <stdint.h>

/* Colour: native little-endian RGB565 so LVGL writes straight into the same
   buffer format the Arduino_Canvas/CO5300 path uses. */
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0

/* Heap: keep LVGL's allocations in PSRAM so we do not eat internal RAM. */
#define LV_MEM_CUSTOM 1
#define LV_MEM_CUSTOM_INCLUDE <esp_heap_caps.h>
#define LV_MEM_CUSTOM_ALLOC(size)        heap_caps_malloc((size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define LV_MEM_CUSTOM_FREE(ptr)          heap_caps_free(ptr)
#define LV_MEM_CUSTOM_REALLOC(ptr, size) heap_caps_realloc((ptr), (size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

/* Tick source (only relevant if LVGL timers ever run; we drive draws manually). */
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE <Arduino.h>
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 0
#define LV_USE_ASSERT_MALLOC 0
#define LV_USE_ASSERT_STYLE 0

/* Fonts. We ship our own Montserrat (src/ui/fonts/lv_font_ui_*.c) instead of
   LVGL's built-ins because the built-ins only cover 0x20-0x7F, 0xB0 and 0x2022
   — the air card needs the micro sign (µ, 0xB5) and superscript three (³, 0xB3),
   which rendered as garbage. Dropping their FontAwesome glyphs also makes the
   font data about half the size.

   Regenerate (from the lvgl package's scripts/built_in_font directory) with:
     npx lv_font_conv@1.5.3 --no-compress --no-prefilter --bpp 4 --size <S> \
       --font Montserrat-Medium.ttf -r 0x20-0x7F,0xB0,0xB3,0xB5,0x2022 \
       --format lvgl -o src/ui/fonts/lv_font_ui_<S>.c --force-fast-kern-format
   then set LV_FONT_UI_<S> 1 below (the generated file is guarded by it). */
#define LV_FONT_MONTSERRAT_8  0
#define LV_FONT_MONTSERRAT_10 0
#define LV_FONT_MONTSERRAT_12 0
#define LV_FONT_MONTSERRAT_14 0
#define LV_FONT_MONTSERRAT_18 0
#define LV_FONT_MONTSERRAT_22 0
#define LV_FONT_MONTSERRAT_24 0
#define LV_FONT_MONTSERRAT_28 0
#define LV_FONT_MONTSERRAT_40 0
#define LV_FONT_MONTSERRAT_48 0

#define LV_FONT_UI_8  1
#define LV_FONT_UI_10 1
#define LV_FONT_UI_12 1
#define LV_FONT_UI_14 1
#define LV_FONT_UI_18 1
#define LV_FONT_UI_22 1
#define LV_FONT_UI_24 1
#define LV_FONT_UI_28 1
#define LV_FONT_UI_40 1
#define LV_FONT_UI_48 1
#define LV_FONT_UI_56 1

#define LV_FONT_CUSTOM_DECLARE   \
  LV_FONT_DECLARE(lv_font_ui_8)  \
  LV_FONT_DECLARE(lv_font_ui_10) \
  LV_FONT_DECLARE(lv_font_ui_12) \
  LV_FONT_DECLARE(lv_font_ui_14) \
  LV_FONT_DECLARE(lv_font_ui_18) \
  LV_FONT_DECLARE(lv_font_ui_22) \
  LV_FONT_DECLARE(lv_font_ui_24) \
  LV_FONT_DECLARE(lv_font_ui_28) \
  LV_FONT_DECLARE(lv_font_ui_40) \
  LV_FONT_DECLARE(lv_font_ui_48) \
  LV_FONT_DECLARE(lv_font_ui_56)

#define LV_FONT_DEFAULT &lv_font_ui_14

#endif /* LV_CONF_H */
