# ESP32 Usage Monitor — AGENTS.md

Waveshare ESP32-S3 AMOLED desk device: shows OpenCode spend/limits, weather,
trend and a clock, and mirrors agent state from an OpenCode plugin over HTTP.
Firmware is PlatformIO/Arduino, ~6.5k lines of hand-written C++ (plus generated
fonts). Host unit tests run on `native`.

## Build (PlatformIO)

```bash
pio run -e esp32-usage-monitor                              # compile
pio run -e esp32-usage-monitor -t upload                    # upload firmware
pio run -e esp32-usage-monitor -t uploadfs                  # upload filesystem
pio run -e esp32-usage-monitor -t upload -t uploadfs        # both
pio run -e esp32-ota -t upload --upload-port <IP>           # firmware, over WiFi
pio test -e native                                          # host unit tests
```

`[env:esp32-ota]` is the same firmware with `upload_protocol = espota`; the
default env stays on USB deliberately. OTA needs the firmware that contains
`src/ota.*` flashed over USB once, after which only the *app* moves — LittleFS
(config + rotated tokens) is untouched, so no `authorize.py` is needed for an
OTA (unlike `uploadfs`). `-t uploadfs` cannot go over the air, and there is no
automatic rollback. The endpoint is password-only (`ota_password`, ≥ 12 chars —
empty/short disables it); `tools/ota.sh [ip]` reads the secret from
`data/config.json`, retries, and `--gen-password` rotates it.

Custom platform (not the default Espressif one): `platform` URL in
`platformio.ini`. Lib versions are pinned exactly so CI and local builds resolve
the same code; bump deliberately.

## Board support

Multiple Waveshare AMOLED boards via `-DBOARD_*` flags in `build_flags`.
Pin dispatch: `src/hw/pins.h` → `src/boards/board_*.h`.
Current: `BOARD_WAVESHARE_ESP32S3_TOUCH_AMOLED_2_16`.

## Local config (gitignored)

`data/config.json` contains auth secrets — **never committed**.
Copy from `data/config.json.example`, then authorise:

```bash
python3 tools/authorize.py          # OAuth device flow, writes refresh_token + org_id
python3 tools/authorize.py --check  # refresh + print current usage (sanity check)
pio run -e esp32-usage-monitor -t uploadfs
```

`workspace_id` (the `wrk_...` org id) is the only required base field. After
editing config, re-run `uploadfs`.

> **⚠️ Token lifecycle.** The refresh token is single-use and *rotates* on every
> refresh; reusing an old one revokes the whole token family (then you must
> re-authorise). The device takes ownership after first boot and persists rotated
> tokens to LittleFS `/tokens.json`.
>
> - `pio run -t upload` (firmware only) is **safe** — it does not touch the filesystem.
> - `pio run -t uploadfs` **erases** LittleFS including `/tokens.json`, so the device
>   falls back to the (now stale) token in `config.json`. **Always run
>   `python3 tools/authorize.py` immediately before an `uploadfs`.** The second
>   `uploadfs` of an unchanged `config.json` is the fatal one, so the target now
>   refuses to run once the token it already pushed is the one still in the file
>   (`tools/uploadfs_guard.py`, state in `.uploadfs-state.json`); the escape hatch
>   is `ESP32_ALLOW_STALE_TOKEN=1`.
> - Never run `authorize.py --check` while the device is live — it rotates the token
>   and invalidates the device's copy.

The macOS arm64 `mklittlefs` workaround (the PlatformIO package ships an
x86_64-only binary and needs an arm64 replacement) is in the README,
"[macOS arm64 note](README.md#macos-arm64-note)".

## Architecture

| Path | Role — touch it when… |
|---|---|
| `src/main.cpp` | Entrypoint: `setup()`/`loop()` wiring, `uiFrame` render gate + idle/dim, power-off, chime dispatch, WiFi/mDNS bring-up. **Tunables live at the top.** |
| `src/poller.{h,cpp}` | Poll schedule + exponential backoff + wedge ladder + AP re-selection trigger. **Pure & host-tested** — change here, not in `main.cpp`, for anything about *when/what* to fetch. |
| `src/data_types.h` | Shared PODs (`UsageData`, `WeatherData`, `MoonData`, `AirData`) + `FetchJob`. Add a payload field here. |
| `src/ui_model.h` | Neutral UI enums (`UiPage`, `UiWeatherCard`) + `UiEnv`. Add a page/card here. |
| `src/ui_compose.{h,cpp}` | Picks page/card(s) for the pager state incl. the two-tile swipe transition. |
| `src/pager.{h,cpp}` | Two-axis gesture + snap/animation state machine. Touch/drag behaviour. |
| `src/usage_display.h` | Public UI drawing API (frame setup, page/card renderers, dots). |
| `src/ui/` | Drawing impl: `cards`, `trend`, `primitives` (fonts + text helpers), `icons`, `lvgl_text`, shared palette `theme.h`. |
| `src/usage_history.{h,cpp}` | RAM-only ring of rolling spend samples feeding the trend page. |
| `src/fetch_task.{h,cpp}` | Background FreeRTOS worker: job/result queues, token hand-off. Threading rules in the header. |
| `src/status_server.{h,cpp}` | HTTP status endpoint (`?state=…`), HMAC auth, chime events. |
| `src/api_client.{h,cpp}` | opencode console REST client (usage + token refresh). |
| `src/weather_client.{h,cpp}` | Weather (wttr.in), moon (MET Norway), air (waqi) transport. Location comes from `weatherSetLocation()`. |
| `src/http_client.{h,cpp}` | Shared **blocking** HTTPS helper: DNS/handshake/read deadlines, CA-bundle verification. |
| `src/parse/` | Header-only, host-testable payload parsers (usage/wttr/moon/air). Pure functions — test them here. |
| `src/util/` | Dependency-free helpers (time/value/interval/trend/loc), host-unit-tested. |
| `src/config.h` | `AppConfig` + LittleFS JSON loader (`/config.json`). Add a config field here. |
| `src/storage.{h,cpp}` | One-shot LittleFS mount helper. |
| `src/token_store.{h,cpp}` | OAuth tokens in `/tokens.json` + config fallback. |
| `src/clock_sync.{h,cpp}` | NTP bootstrap + RTC resync; `clockSyncValid()` gates all TLS. |
| `src/wifi_setup.{h,cpp}` | WiFi config UI (scan list, button-based password entry). |
| `src/frame_dump.{h,cpp}` | `-DFRAME_DUMP` screen walker streaming canvases over USB CDC. |
| `src/diag.{h,cpp}` | **TEMPORARY** freeze diagnostics (RTC breadcrumbs + stall reporter). Still load-bearing — see invariants. |
| `src/ota.{h,cpp}` | Wireless firmware update (ArduinoOTA). Pauses the pollers, paints the progress screen, flushes tokens before the reboot. |
| `src/hw/` | Drivers: `display`, `input`, `power`, `rtc`, `net`, `expander`, `pins.h`. |
| `src/voice/` | I2S output (ES8311) + boot-synthesised `chime.*`. |
| `src/boards/` | Board-specific pin definitions. |
| `fonts/` | **Generated** LVGL Montserrat fonts (see `docs/text-lvgl.md`). Never edit; excluded from search via `.ignore`. |
| `test/` | Host unit tests (`pio test -e native`): `test_util` + `test_poller`. |
| `tools/` | PC-side Python tools, the OpenCode plugin, and `build_fonts.py`. |
| `docs/` | Subsystem deep dives (see below). |

## Invariants

Hard rules — breaking one of these has already caused a real bug. Read the
linked deep dive before changing the area.

- **All text goes through LVGL** (Montserrat 4bpp via `src/ui/lvgl_text.*`), and
  the text helpers take/return *logical* units — they multiply by `UI_SCALE`
  internally. u8g2 is gone; do not reintroduce 1-bit fonts.
- **Rendering is state-driven and full-frame.** Re-render only when
  `uiFrameKey()` changes; push the whole 368×448 window in one QSPI transaction.
  No dirty rectangles, and don't raise the QSPI clock past the 40 MHz default.
- **The panel is only dimmed, never blanked** (idle, or otherwise), so it stays
  glanceable. No IMU/gestures — touch + the keys (Key1 cycles; the left/right
  side keys step the page axis).
- **Network fetches only run on the fetch task** (`src/fetch_task.*`). `loop()`
  schedules and harvests. Never fetch inline; never touch the display, I2C or
  LittleFS from the fetch task (flash writes come back via
  `fetchTaskPendingSave()`).
- **Every blocking phase must be bounded.** `setHandshakeTimeout()` takes
  **seconds** (not ms); DNS gets its own deadline. Worst case per request is
  bounded — see `docs/threading-fetch.md`.
- **Chain-verify outbound TLS** against the ESP-IDF CA bundle. There is no
  `setInsecure()` — do not reintroduce it.
- **A plausible clock is required before the first fetch** (cert validity dates).
  Keep the poll schedule gated on `clockSyncValid()`, and keep the offset passed to
  `clockSyncSetOffset()` (config `tz_offset_min`) consistent with what
  `hwRtcWrite()` stores — the RTC holds *local* wall time, so the two halves have
  to agree or the boot seed is off by the difference until NTP corrects it.
- **Radio: `MIN_MODEM` while idle, awake for each fetch, `WIFI_POWER_11dBm`.**
  `MAX_MODEM` was measured out — every poll timed out. The knobs are
  `APP_WIFI_TX_DBM` and `netRadioWake()`/`netRadioRest()`; re-measure
  `http: … tls=…` with `tools/link_probe.py` before changing either, and read the
  boot line's `bssid=` first — a stale panel is at least as likely to be the AP
  as the firmware (docs/power.md).
- **One fetch job in flight.** A poller timer is only marked run once
  `fetchTaskStart()` accepts it. Keep the poll policy in `src/poller.cpp` and
  keep `test/test_poller` green.
- **Roaming keys on transport health, not rssi.** The trigger is `ROAM_FAILS`
  consecutive fetches that died on the path with no success in between
  (`httpTransportFailed()`), and the actor is `netRoam()` pinning a *different*
  BSSID of our own SSID. Do not add an rssi threshold to it: in the sample that
  motivated it the dead AP measured inside the good AP's band, and the whole
  point is that rssi cannot tell them apart. See docs/network-polling.md.
- **The boot PWRON press must not power the board off.** `loop()` consumes the
  AXP IRQ the boot press latched and only acts on a fresh one.
- **`uploadfs` erases the device's rotated tokens.** Run `authorize.py` first
  (see the token-lifecycle warning above); the `uploadfs` guard enforces it.
- **`src/diag.*` is temporary but load-bearing** — it is what makes a wedged
  fetch or a frozen UI diagnosable over USB CDC. Don't delete it while the
  watchdog still relies on its breadcrumbs.
- **OTA only replaces the app.** `ArduinoOTA::handle()` blocks `loop()` for the
  whole transfer and writes flash, so it runs on the loop task only (never the
  fetch task), it pauses every poller for its duration, and the token flush
  before its reboot is what keeps the rotated refresh token alive. The
  partition table and LittleFS are USB-only; there is no rollback. The endpoint
  never runs unauthenticated: a missing/short `ota_password` disables it.

## Deep dives (docs/)

Read the relevant one before changing that area:

- [docs/ui-navigation.md](docs/ui-navigation.md) — pages, weather cards, trend page, no-IMU note.
- [docs/power.md](docs/power.md) — power key, display power/heat, dim rules, radio.
- [docs/text-lvgl.md](docs/text-lvgl.md) — the LVGL rasteriser, fonts, `fonts/` build hook.
- [docs/threading-fetch.md](docs/threading-fetch.md) — fetch task, bounded blocking, safety nets.
- [docs/network-polling.md](docs/network-polling.md) — poll periods, idle backoff, WiFi scan.
- [docs/networking-tls.md](docs/networking-tls.md) — TLS/CA bundle, console API + auth, throughput.
- [docs/pc-bridge.md](docs/pc-bridge.md) — status endpoint, HMAC auth, chimes, plugin.
- [docs/debug-frame-dump.md](docs/debug-frame-dump.md) — `-DFRAME_DUMP` screen walker.

## PC-side tools

- `tools/authorize.py` — opencode console device-flow authoriser + `--check`
- `tools/link_probe.py` — sample the USB serial and report link health (attempts,
  per-host `tls=`, failures, rssi, per-AP split); `--cycles` resets the board per
  sample to get a failure rate in minutes
- `tools/uploadfs_guard.py` — `uploadfs` pre/post hook that refuses to flash a
  `config.json` token the device has already rotated away (run it bare to ask the
  same question by hand); see the token-lifecycle warning above
- `tools/opencode-plugin/` — OpenCode V2 plugin that drives the status endpoint
- `tools/status-ping.sh` — signed status ping, for testing the device by hand
- `tools/status_stub.py` — local stand-in for the status endpoint (tests the plugin + HMAC)
- `tools/check-hosts.sh` — verify the firmware's CA bundle still covers every host it talks to
  (reads the bundle out of the *built* library). Exits non-zero when a host needs a firmware update.
- `tools/build_fonts.py` — PRE `extra_scripts` hook that compiles `fonts/`.

## Tests / CI

CI (`.github/workflows/ci.yml`) runs `pio test -e native` and
`pio run -e esp32-usage-monitor` on every push/PR.

Host tests need **no hardware** and cover the dependency-free logic:

- `test/test_util/` — `src/util/` (epoch/date parsing, weather-icon kind, moon
  phase, AQI label, `Interval`, trend math) and the header-only `src/parse/`
  parsers (usage meters, wttr one-line, moon/air JSON).
- `test/test_poller/` — `src/poller.cpp` (selection priority + gates, backoff
  caps, cooldowns, the wedge ladder). `[env:native]` builds just that one
  firmware source via `test_build_src` + `build_src_filter`.

`platformio.ini` keeps the shared ESP32 settings in `[esp32_base]` (extended by
`[env:esp32-usage-monitor]`) so `[env:native]` does not inherit
`framework = arduino`; the native env instead pulls ArduinoJson only (needed by
`src/parse/`) plus `-Isrc -Isrc/util`.
