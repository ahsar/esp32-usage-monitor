# Text rendering (LVGL)

Text is anti-aliased with **LVGL used purely as an offscreen rasteriser**: an
`lv_canvas` points at the existing (PSRAM) Arduino_Canvas framebuffer and
`lv_canvas_draw_text()` blends Montserrat 4bpp glyphs straight into it. No LVGL
widgets, screens or driven display — `src/ui/lvgl_text.*` registers a tiny dummy
display (LVGL needs one before any object exists), creates the canvas once, and
exposes `lvglDrawText()` / `lvglTextWidth()`.

**u8g2 is no longer a dependency**: the old 1-bit fonts looked jagged on the
314-PPI panel, and every text draw (cards, WiFi setup, the shutdown banner and
the `die()` report) goes through LVGL now.

- `src/ui/primitives.cpp` owns the five fonts (`F_BIG..F_SMALL`) and the
  `textAt`/`textRight`/`textCenter` helpers that all card code uses. **The helpers
  take/return logical units and multiply positions by `UI_SCALE` internally** —
  the LVGL fonts are already sized for the canvas, so getting this wrong shifts
  every string by half a screen.
- The fonts are **our own** Montserrat builds in `fonts/lv_font_ui_<S>.c`,
  generated with `lv_font_conv` (recipe in `include/lv_conf.h`), *not* LVGL's
  built-ins: those only cover `0x20-0x7F`, `0xB0` and `0x2022`, so the air
  card's `µg/m³` (0xB5 / 0xB3) rendered as garbage. Each generated file is
  guarded by `LV_FONT_UI_<S>`, which must be `1` in `lv_conf.h`.
- **`fonts/` is outside `src/` on purpose.** The 11 font files are ~1.6 MB /
  ~29k lines of generated glyph tables, and PlatformIO only auto-collects
  sources under `src/`, so `tools/build_fonts.py` (a PRE `extra_scripts` hook in
  `platformio.ini`) adds them to the build. A repo-root `.ignore` also keeps
  them out of ripgrep/fd results. **Never edit anything in `fonts/` by hand** —
  regenerate with the `lv_font_conv` recipe above.
- Everything else (bars, icons, moon, dots, the 555 logo) is still drawn directly
  with Arduino_GFX into the same buffer; only *text* goes through LVGL.
- `ScaledCanvas` (`hw/display.h`) routes its own `setFont`/`setCursor`/`print`/
  `printf` through LVGL too, so the WiFi setup screen and the shutdown banner
  share the same pipeline. Its cursor is a logical baseline and `print()`
  advances by the rendered width, matching the old u8g2 behaviour.
- `lvglTextInit()` is called at the end of `hwDisplayInit()` (the framebuffer must
  exist first), so LVGL is ready for the `die()` report on any failure after the
  panel is up; before that the canvas is null and `lvglDrawText()` no-ops.
