# Changelog

All notable changes to this project are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **A signed settings channel on the status endpoint.** The chime, the awake
  brightness and the whole location can now be changed over the network instead
  of by editing `data/config.json` and running `uploadfs` (which erases the
  device's rotated OAuth token — see the guard below):

  ```
  tools/status-settings.sh                        # what is overridden right now
  tools/status-settings.sh chime=0 brightness=3
  tools/status-settings.sh place=Shanghai lat=31.2304 lon=121.4737
  tools/status-settings.sh place=                 # clear: back to config.json
  ```

  `src/parse/settings_parse.h` is the whole decision — key names, ranges,
  "lat and lon belong together", clearing, and the "a typo changes nothing"
  all-or-nothing rule — and is host-tested (16 new cases); `src/settings_store.*`
  persists the overrides to `/settings.json` and overlays them on `AppConfig` at
  boot, so they survive a reboot and clearing one restores the configured value
  without `config.json` ever being rewritten (it is the packed file the uploadfs
  guard reasons about). Accepted changes are applied live: chime, brightness, the
  request paths, an NTP resync for a new timezone, and a forced round of polls so
  the panel does not sit on the old place until the next weather period. A
  settings request is signed over the **raw query string** rather than a single
  value (so every field is bound to the key), while `?state=` keeps its original
  value-only signature and the OpenCode plugin is untouched; with no `status_key`
  the channel answers `403` instead of accepting an unauthenticated write.
  `tools/status-settings.sh` is the client and `tools/status_stub.py` now checks
  both signature forms, so the tool is testable without hardware.
- **The location is config, not code.** `place`, `lat`/`lon`, `tz_offset_min`,
  `waqi_station` and `air_label` now come from `config.json` instead of being
  written into the firmware: `place` is both the wttr.in query and the status-bar
  title, `lat`/`lon`/`tz_offset_min` are what the MET Norway request and the
  PCF85063 wall time are built from, and `waqi_station` (+ `air_label`) selects
  the aqicn.org feed the Air card reads. The defaults are the values the firmware
  used to hard-code (Beijing, 39.9042/116.4074, UTC+8, `@3303`), so an existing
  `config.json` without the block behaves exactly as before. The request paths
  that carry them are built by a new host-tested `src/util/loc_util.h`
  (percent-encoding, the `%2B08:00` offset form, and the two API paths), with
  tests in `test/test_util`. `clockSyncSetOffset()` now installs the timezone, and
  the RTC seed moved *after* `loadConfig()` so the boot clock is interpreted with
  the configured offset rather than the compiled-in one.
- **`uploadfs` guard (`tools/uploadfs_guard.py`).** `uploadfs` erases LittleFS,
  `/tokens.json` with it, so the rebooted device falls back to the `refresh_token`
  in `data/config.json` — which is only still usable if the device has *not*
  booted since it was written. Push a config the device has already claimed and
  the fallback refresh gets `invalid_grant`, which revokes the whole OAuth family
  and costs a full `authorize.py` round trip; on screen it just looks like the
  meters went empty, which is a long way back to the cause. Wired into
  `platformio.ini` as a PRE/POST action on the `uploadfs` target (state in
  `.uploadfs-state.json`, host-side only — everything under `data/` is packed into
  the image), it blocks the flash when the token in `config.json` is the one last
  pushed, and records the pushed token after a successful upload: the second
  `uploadfs` of an unchanged config is the fatal one, and that is precisely the
  case it can see. `ESP32_ALLOW_STALE_TOKEN=1` bypasses it for a deliberate
  re-flash, and running the script bare answers the same question without
  uploading. It cannot know about tokens pushed before it existed, so the very
  first upload of an already-claimed config still needs `authorize.py`.
- **Radio diagnostics and a link probe.** `diagSetNet()` now carries the RSSI, so
  every periodic line ends in `rssi=`, and the boot line prints `bssid=` beside
  the IP — the two numbers that separate "the link is fading" from "we are on a
  different AP". `tools/link_probe.py` samples the USB serial and reports
  attempts, per-host `tls=` spread, failures, rssi range, fetches per boot and a
  per-AP split; `--cycles` resets the board once per sample (boot forces all four
  pollers) to buy a failure rate in minutes rather than one attempt a minute. It
  reports its own blind spots too: windows that roamed between APs, and reset
  windows that censor attempts slower than the window. It paid for itself on the
  first run: the "stale panel" episodes turned out to be an AP the device
  associates with and then cannot pass TCP/TLS through, not a firmware fault —
  see `docs/power.md` for how to read the `bssid=` that proves it.
- **AP re-selection (roaming).** An ESP32 has no 802.11k/v/r, so it stays on
  whichever BSSID it picked until the link dies — which is how a distant office
  AP that associates and then carries nothing can strand the device for hours
  with a stale panel. After `ROAM_FAILS` (3) consecutive fetches that died on the
  path with no success in between, `loop()` scans and re-associates pinned to the
  strongest *other* BSSID of our own SSID (`netRoam()`, falling back to un-pinned
  selection if that AP has vanished), then holds off for `ROAM_COOLDOWN_MS`
  (3 min) whether or not it moved. The trigger is transport health and
  deliberately not rssi — the dead AP in that sample sits inside the good AP's
  band — and a 5xx does not count towards it, because it proves the AP carried
  the request. The policy is host-tested alongside the backoff ladder.

### Changed

- **The station is awake for the duration of each fetch**
  (`netRadioWake()`/`netRadioRest()`, `hw/net.cpp`). `MIN_MODEM` still sleeps
  between DTIM beacons and that wait is paid on every round trip of a TLS
  handshake; the radio does not sleep for the ~5 s a fetch actually uses the
  link. Measured on one AP: opencode.ai `tls` median 3043 ms → 2159 ms and 3/26
  failed → 0/24. One arm each and at different times, so treat it as
  re-measure-me rather than proven (docs/power.md).
- **`APP_WIFI_TX_DBM`** replaces the inline `WIFI_POWER_11dBm`, so a weak-signal
  site can raise TX power from `platformio.ini` instead of editing `main.cpp`.
  Default unchanged: that sample found no correlation between rssi and failures,
  and the failures it did find were an AP that never completes a connect.

### Fixed

- **A failed weather poll no longer costs 30 minutes.** `pollerResult()` applied
  only a cooldown and never re-armed the job's own timer, so the gap between
  attempts was `max(cooldown, interval)`. Since the weather/air/moon intervals
  (30 min / 60 min / 12 h) dwarf anything the backoff ladder can produce, the
  ladder was dead code for every job but the 60 s usage meter: one transient
  failure on the burst that follows association meant 30 minutes of "no weather"
  while opencode's meters repaired themselves in a minute, and the only fast fix
  was a reconnect (which runs `pollerForceAll()`). A failure now re-arms the
  timer, so the cooldown paces the retry — 60 s after the first failure,
  doubling to a 16 min ceiling — and the cooldown still gates it, so this cannot
  hot-loop (`test_failure_rearms_timer`). wttr.in and MET Norway also get three
  attempts per poll instead of one, to absorb the cheap transient kind (a 503
  burst, an empty body); that is bounded at ~170 s against the 240 s watchdog
  deadline, the same order as the usage poll that refreshes its token twice.

## [0.3.0] - 2026-10-15

### Added

- **Wireless firmware update (OTA).** The build already had the two-slot
  partition table, but nothing to receive an image on it. `src/ota.{h,cpp}` now
  runs an ArduinoOTA server, so `pio run -e esp32-ota -t upload
  --upload-port <ip>` replaces the app over WiFi — LittleFS (config + the rotated
  OAuth tokens) is untouched, which is the whole point: unlike `uploadfs`, an OTA
  needs no `authorize.py` and cannot invalidate the device's token. The default
  env keeps using USB (`upload_protocol` only changes in `[env:esp32-ota]`).
  Because `ArduinoOTA::handle()` blocks `loop()` for the entire transfer, the
  pollers are paused for its duration (new `pollerPauseAll()`, host-tested) so
  the fetch worker cannot fight the push for the radio, the panel is painted from
  the progress callback (`Updating …%`), the radio is raised to `WIFI_PS_NONE`
  for the transfer only, and pending state — a refresh token that rotated
  mid-transfer — is flushed before the reboot. The progress repaint is throttled
  to one frame per percent: `onProgress` fires per ~1 KB chunk, so repainting
  every chunk meant ~1600 full-frame QSPI pushes per update. The receive loop
  also waits 10 s for the next chunk instead of the 1 s default — a weak link
  drops a segment, TCP backs off, the host blocks in `sendall`, and the default
  timeout aborted transfers that were still perfectly healthy.
- `ota_password` in `config.json` — **required**, at least 12 characters, or the
  OTA server does not start (an unauthenticated firmware-write endpoint on the
  LAN is not a default). espota gets the same secret via `OTA_AUTH`, which
  `tools/ota.sh` (also `--gen-password`) fills in from `config.json`.

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
- **Holding the middle (PWR) key no longer powers the board straight back off.**
  The key is the AXP `PWRON` input, so the press that switched the device on also
  latched the AXP long-press IRQ while the board was still booting, and `loop()`
  shut it down on its first pass — the screen flashed, then went black. `loop()`
  now consumes the IRQ the boot press produced and only acts on a fresh one. The
  AXP PWRON timers were retuned (ON 128 ms / OFF 10 s / IRQ 2.5 s) so a slow
  power-on hold is no longer hard-cut at 4 s.
- Releasing `Key1` during the boot WiFi connect no longer aborts it: on the 2.16
  boards `Key1` is the PWR key, and the setup screen treated its release as
  "choose another network" — which scanned (disconnecting the in-flight
  `WiFi.begin()`) instead.

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
- **The poll schedule and failure policy moved out of `main.cpp` into
  `src/poller.{h,cpp}`** (timers, selection priority, exponential backoff, the
  wedge ladder). It is dependency-free, so it is now covered by
  `test/test_poller/` instead of only being readable; `main.cpp` keeps the actor
  side (restarting the fetch task) and the wiring.
- **The payload structs moved into `src/data_types.h`** (with `FetchJob`), so
  the UI layer, the pager and the header-only parsers no longer include the
  network client headers — and, through them, the OAuth token types. `apiFetch*`
  still live in the clients; only the data crossed over.
- **The generated Montserrat fonts moved from `src/ui/fonts/` to `fonts/`.**
  PlatformIO only auto-collects `src/`, so `tools/build_fonts.py` (a PRE
  `extra_scripts` hook) adds them back to the build; a repo-root `.ignore` keeps
  ~29k generated lines out of ripgrep/fd results.
- `AGENTS.md` was split: it keeps the build commands, the module map and the hard
  invariants, and the subsystem deep dives now live under `docs/` (linked from
  it) instead of inflating the file every agent session loads.

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
- **Trend page.** A fourth page plots rolling-window spend: current cost,
  burn rate (`$/h`), time-to-limit, a cumulative curve and per-poll bars, from
  the RAM-only ring in `src/usage_history.*`. Cost math is in
  `src/util/trend_util.h` (host-tested); `UsageData` now carries the
  `*MicroCents` fields.
- New `config.json` toggle: `chime`.
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
- Ship our own Montserrat builds (`fonts/`, via `lv_font_conv`) so the
  glyphs the UI actually needs are present: LVGL's built-ins only cover ASCII +
  `°` + `•`, which left the air card's `µg/m³` rendering as garbage. Dropping
  their FontAwesome glyphs also keeps the font data about half the size.
- `tools/frame_dump.py`: rebuild the `-DFRAME_DUMP` screen walk as PNGs for
  host-side layout checks.

### Removed

- **The 555 easter egg.** `src/easter_egg.*` and `data/555_audio.raw` are gone;
  nothing plays on a shake any more. The "Thinking…" animation keeps the 555
  artwork.
- **All IMU/gesture handling.** `src/gestures.*`, `src/hw/imu.*` and
  `src/util/gesture_util.h` are gone, the QMI8658 is no longer initialised, and
  the `imu_gestures` config toggle is dropped. Face-down blanking was the only
  path that could blank the panel while the firmware kept running, and a false
  positive (`hwDisplayBrightnessRaw(0)` with touch/key disabled) was
  indistinguishable from a dead device — the panel is now only ever dimmed.
  Navigation is touch + Key1; `EV_SHAKE`/double-tap/pickup/rotate are gone too.

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

[Unreleased]: https://github.com/ahsar/esp32-usage-monitor/compare/v0.3.0...HEAD
[0.3.0]: https://github.com/ahsar/esp32-usage-monitor/releases/tag/v0.3.0
[0.1.0]: https://github.com/ahsar/esp32-usage-monitor/releases/tag/v0.1.0
