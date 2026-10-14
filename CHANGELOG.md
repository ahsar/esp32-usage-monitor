# Changelog

All notable changes to this project are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed

- **A blocked host could stop every poller for hours.** `setHandshakeTimeout()`
  takes *seconds* (the core multiplies by 1000 and compares against `millis()`),
  so the 15000 passed by `http_client.cpp` asked for 4 h 10 m instead of 15 s.
  A host that accepts TCP but never completes the TLS handshake (a filtered or
  blackholed endpoint) therefore wedged the single fetch worker far beyond every
  other timeout, freezing the usage meter with no error shown. The cap is now a
  named `HANDSHAKE_TIMEOUT_S`.
- Every blocking phase of an HTTPS request is bounded and logged
  (`dns=… tls=… read=…`). DNS is resolved first with its own 8 s deadline —
  `lwip_getaddrinfo` (inside `client.connect(host, …)`) takes no timeout at all.
- A fetch that fails no longer retries every cycle: each poller backs its host
  off (1 min doubling, capped at 2 min for usage and 30 min for weather/moon/air).
- A worker stuck inside an SDK call that ignores the phase timeouts is now
  detected by its job age (4 min) and restarted, with the offending host paused,
  instead of silently freezing the whole poll loop forever.

### Changed

- **Wi-Fi power save is `MIN_MODEM` for good.** `MAX_MODEM` looked attractive
  for idle radio power, but the dozing radio loses TLS handshake packets: at the
  same spot minutes apart, MIN_MODEM gave handshakes of 2.2-4.4 s with all four
  pollers succeeding, while MAX_MODEM gave 4.5/11.7/31.7/30.7 s with every poll
  timing out. Fetching is this device's whole job and it is desk hardware.
- `wttr.in`/moon/air are fetched with one attempt per poll instead of two:
  retries now live in the scheduler's back-off, and the single job slot is
  released sooner.
- The 10 s diagnostic line carries the in-flight job, its age and the *worker's*
  own phase (`job=… age=… fbc=… host=…`, kept apart from the loop's breadcrumb,
  which re-rendering overwrites), so a stalled fetch says where it is stalled.

### Added

- **PC bridge.** `tools/opencode-plugin/` is an OpenCode V2 plugin that pushes
  agent activity to the device (`?state=running|waiting|done|failed`), and
  `tools/status-ping.sh` does the same by hand. Without it nothing ever drove
  the status server. The device now advertises `usage-monitor.local` over mDNS
  so the plugin does not need a DHCP IP. The status server gained a `waiting`
  state (OpenCode's `permission.asked`) rendered as a "Needs you" animation.
- **Completion chimes.** `src/voice/chime.*` synthesises three short PCM
  cues at boot (rising = done, falling = failed, double blip = needs you) and
  streams them through the existing ES8311 path. No raw file, so activating a
  chime never needs an `uploadfs` (which would erase the rotated tokens).
- **IMU gestures.** `src/gestures.*` is now the single IMU sampling point
  (shake detection moved out of `easter_egg.cpp`): double-tap pages, pickup
  wakes, face-down blanks the panel, and a gyro twist steps pages/cards. The
  pure classifier lives in `src/util/gesture_util.h` and is host-tested.
- **Trend page.** A fourth page plots rolling-window spend: current cost,
  burn rate (`$/h`), time-to-limit, a cumulative curve and per-poll bars, from
  the RAM-only ring in `src/usage_history.*`. Cost math is in
  `src/util/trend_util.h` (host-tested); `UsageData` now carries the
  `*MicroCents` fields.
- New `config.json` toggles: `chime`, `imu_gestures`.
- **Status-endpoint HMAC auth.** The port-80 endpoint now requires an
  HMAC-SHA256 signature (`X-Sig` / `X-Ts`) over `"<state>|<unixSeconds>"`,
  verified in constant time against `status_key` from `config.json`, with a
  ±60 s replay window once NTP is up. Bad signatures get `401` and change
  nothing. Empty key = auth disabled (logged at boot). The plugin signs with
  `node:crypto`; `tools/status-ping.sh` signs too, and `tools/status_stub.py`
  is a local stand-in for testing.
- **Outbound TLS is now verified.** `http_client.cpp` no longer calls
  `setInsecure()`; it validates the chain against the ESP-IDF CA bundle, so an
  active MitM on the LAN can no longer impersonate `opencode.ai` and steal the
  OAuth tokens. Adds ~70 KB of flash. To keep certificate-date checking working,
  the system clock is now seeded from the PCF85063 at boot
  (`clockSyncRestoreFromRtc()`) and the poll schedule is gated on
  `clockSyncValid()`.
- `tools/check-hosts.sh` — reads the CA bundle back out of the built firmware
  and checks that every host the device talks to still chains to a bundled
  anchor, so a CA switch shows up as a failed check instead of a silently STALE
  panel. Exits non-zero when a firmware update is needed.

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
