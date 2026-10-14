# Changelog

All notable changes to this project are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Repository governance: `LICENSE` (MIT), `CONTRIBUTING.md`, issue/PR templates,
  firmware version string (`src/version.h`) printed in the boot log.
- Configurable network poll periods in `data/config.json`
  (`poll_usage_s`, `poll_usage_idle_s`, `poll_weather_s`, `poll_air_s`,
  `poll_moon_s`) with an idle back-off for the usage meter.
- Anti-aliased text. `src/ui/lvgl_text.*` uses LVGL as an offscreen
  `lv_canvas` rasteriser over the existing framebuffer, so all text — the cards,
  the WiFi setup screen, the shutdown banner and the `die()` report — is 4bpp
  Montserrat smooth instead of jagged 1-bit u8g2 glyphs. Layout, shapes,
  animations and the pager are unchanged.
- Ship our own Montserrat builds (`src/ui/fonts/`, via `lv_font_conv`) so the
  glyphs the UI actually needs are present: LVGL's built-ins only cover ASCII +
  `°` + `•`, which left the air card's `µg/m³` rendering as garbage. Dropping
  their FontAwesome glyphs also keeps the font data about half the size.
- `tools/frame_dump.py`: rebuild the `-DFRAME_DUMP` screen walk as PNGs for
  host-side layout checks.

### Changed

- Display: replace dirty-region pushes with state-driven, full-frame pushes.
  `uiFrame()` now re-renders only when the visible UI state actually changes
  (`uiFrameKey()`), and `hwDisplayPush()` always blits the whole letterbox
  window in one QSPI transaction. This removes the dirty-rect shadow, whose
  desync produced accumulating artefacts when swiping between pages. (An 80 MHz
  QSPI clock was tried and reverted: it intermittently wedges the bus on this
  CO5300, so the default 40 MHz is kept.)
- WiFi: max modem sleep (`WIFI_PS_MAX_MODEM`) and slower weather/air/moon poll
  periods (30 min / 60 min / 12 h) to cut radio wake-ups and heat.
- Add `lvgl/lvgl` 8.4.0 and configure it as a text rasteriser only
  (`include/lv_conf.h`); the host `native` env ignores it (`lib_ignore`).
- Text now covers the WiFi setup screen, the shutdown banner and the `die()`
  report as well: `ScaledCanvas`'s `setFont`/`setCursor`/`print`/`printf` are
  LVGL-based, and `lvglTextInit()` moved into `hwDisplayInit()`. The big display
  font is 56 px (the clock/temperature).
- Docs: correct the `src/ui/` / `usage_display.h` project-structure entries and
  the test-coverage notes in `README.md` / `AGENTS.md`.
- Pin all PlatformIO library dependencies to exact versions for reproducible
  builds, and add an `.editorconfig`.

### Fixed

- WiFi setup screen was drawn but never pushed to the panel: `uiFrame()` returned
  before `hwDisplayPush()`, so the boot splash stayed on screen for the whole
  connect/scan phase (indefinitely if the saved network was unreachable).

### Testing

- Expand the host unit tests with parser boundary/robustness cases (truncated
  JSON, non-object meters, numeric vs string fields, over-long fields).

### Removed

- `compile_commands.json` is no longer tracked (build artifact).
- `src/util/dirty_rect.h` and its host tests (dirty-region diffing removed).
- The `U8g2` dependency (its last users — the WiFi setup screen, the shutdown
  banner and `die()` — now render through LVGL).

## [0.1.0] - 2026-10-09

Initial tracked baseline: three-page UI (Weather / Usage / Clock) on the Waveshare
ESP32-S3-Touch-AMOLED-2.16, OAuth device flow, WiFi setup UI, dirty-region display
pushes, and host unit tests for the pure helpers and parsers.

[Unreleased]: https://github.com/ahsar/esp32-usage-monitor/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/ahsar/esp32-usage-monitor/releases/tag/v0.1.0
