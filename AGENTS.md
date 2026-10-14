# ESP32 Usage Monitor — AGENTS.md

## Build (PlatformIO)

```bash
pio run -e esp32-usage-monitor                              # compile
pio run -e esp32-usage-monitor -t upload                    # upload firmware
pio run -e esp32-usage-monitor -t uploadfs                  # upload filesystem
pio run -e esp32-usage-monitor -t upload -t uploadfs        # both
```

Custom platform (not the default Espressif one): `platform` URL in `platformio.ini`.

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
>   `python3 tools/authorize.py` immediately before an `uploadfs`.**
> - Never run `authorize.py --check` while the device is live — it rotates the token
>   and invalidates the device's copy.

### macOS arm64 note

The PlatformIO `tool-mklittlefs` packages ship an x86_64-only binary, so `uploadfs`
fails with `mklittlefs: Bad CPU type in executable`. Replace it with an arm64 build
from <https://github.com/jason2866/mklittlefs/releases> (asset
`aarch64-apple-darwin-*.tar.gz`) at
`~/.platformio/packages/tool-mklittlefs@3.2.0/mklittlefs`.

## Architecture

| Path | Role |
|---|---|
| `src/main.cpp` | Entrypoint: setup/loop, network polling schedule, input wiring, display power |
| `src/ui_compose.{h,cpp}` | Picks page/card(s) for the current pager state and drives the display layer |
| `src/ui_model.h` | Neutral UI enums (`UiPage`, `UiWeatherCard`) + `UiEnv`, shared by pager/UI |
| `src/frame_dump.{h,cpp}` | `-DFRAME_DUMP` screen walker (streams raw canvases over USB CDC) |
| `src/pager.{h,cpp}` | Two-axis page/weather-card gesture + animation state machine |
| `src/status_server.{h,cpp}` | HTTP status server (`?state=running\|waiting\|done\|failed`) |
| `src/clock_sync.{h,cpp}` | NTP bootstrap + RTC resync scheduling |
| `src/gestures.{h,cpp}` | Single IMU sampling point: shake / double-tap / pickup / face-down / rotate |
| `src/easter_egg.{h,cpp}` | 555 audio playback (shake detection lives in `gestures.cpp`) |
| `src/usage_history.{h,cpp}` | RAM ring of rolling-window spend samples (trend page) |
| `src/storage.{h,cpp}` | LittleFS mount helper (one-shot, format-on-fail) |
| `src/config.h` | `AppConfig` struct + LittleFS JSON loader (`/config.json`) |
| `src/token_store.{h,cpp}` | Runtime OAuth tokens persisted to LittleFS (`/tokens.json`) + config fallback init |
| `src/http_client.{h,cpp}` | Shared blocking HTTPS helper (headers, HTTP/1.0, timeouts) |
| `src/fetch_task.{h,cpp}` | Background fetch task: job/result queues + rotated-token hand-off |
| `src/api_client.cpp` | opencode console REST client (`https://opencode.ai/console/api/go/status`) |
| `src/weather_client.{h,cpp}` | Weather (wttr.in), moon (MET Norway), air quality (waqi) transport |
| `src/parse/` | Header-only, host-testable payload parsers (usage/moon/air) |
| `src/usage_display.h` | Public UI drawing API (frame setup, page/card renderers, dots) |
| `src/ui/` | UI drawing impl (`cards`, `primitives`, `icons`, `lvgl_text`) + shared palette (`theme.h`) |
| `src/util/` | Dependency-free helpers (time/value/interval/gesture/trend, host-unit-tested) |
| `src/wifi_setup.cpp` | WiFi config UI (button-based password entry) |
| `src/hw/` | Drivers: display, input, power, IMU, RTC, net |
| `src/boards/` | Board-specific pin definitions |
| `src/voice/` | I2S audio output (ES8311 codec, `chime.*` cue synthesis) |
| `test/` | Host unit tests (`pio test -e native`) |
| `tools/` | PC-side Python tools + the OpenCode plugin (`tools/opencode-plugin/`) |

### UI / navigation

- Horizontal pages: **Weather ↔ Usage ↔ Trend ↔ Clock**.
- Weather has vertical cards: **Now → Sun → Air → Moon → Moonrise**.
- Drag follows the finger and snaps; tap advances; Key1 cycles; **double-tap
  also advances**, and **twisting the device left/right steps pages/cards**
  (`gestures.cpp`). Dots: bottom row = pages, right column = weather cards.
- All on-screen copy is English. Text is rendered with **LVGL Montserrat** (4bpp
  anti-aliased) via `src/ui/lvgl_text.*`; the old 1-bit u8g2 fonts looked jagged
  on the 314-PPI panel. **u8g2 is no longer a dependency** — every text draw
  (cards, WiFi setup, the shutdown banner and the `die()` report) goes through
  LVGL now.
- Weather cards reserve the right `WC_R` margin so content never collides with the
  card dots.

### PC bridge, chimes and gestures

The device never talks to OpenCode; OpenCode talks to the device. `status_server`
is a plain HTTP server on port 80 and `tools/opencode-plugin/` (OpenCode **V2**
plugin, `session.execution.*` / `permission.asked` events) is the client:

| Request | State | Device |
|---|---|---|
| `?state=running` | agent working | "Thinking…" animation, wakes the panel |
| `?state=waiting` | permission/question | "Needs you" animation + attention chime |
| `?state=done` / `?state=failed` | finished / errored | rising / falling chime |

`done`/`failed` only chime on a real transition, and never for the Key1 dismiss,
so hand-dismissing the animation stays silent. Do **not** rely on V1 event names
(`session.idle`, `session.status`, `permission.updated`) — V2 does not emit them.
`tools/status-ping.sh` drives the same endpoint by hand, so the firmware path can
be tested without OpenCode. The device advertises `usage-monitor.local` over
mDNS (`ESPmDNS`) so the plugin needs no DHCP IP.

**Auth.** Port 80 is reachable by anyone on the LAN, so the endpoint takes an
HMAC-SHA256 signature over `"<state>|<unixSeconds>"` in `X-Sig` / `X-Ts`
(`status_server.cpp`), verified in constant time against the `status_key` from
`config.json`. With the clock NTP-synced, a ±60 s window blocks replay; a bad or
missing signature gets `401` and changes nothing. The transport stays plain HTTP,
but the key never goes on the wire (only the signature and the timestamp), so it
is not leaked by sniffing; what plain HTTP allows is replaying a *captured*
request inside the 60 s window. TLS (an mbedTLS server on the ESP32) is the
heavier follow-up if that matters. **With an empty
`status_key` auth is disabled** and the boot log says so. `tools/status_stub.py`
is a local stand-in for the endpoint that reimplements the check, so the plugin
can be exercised without hardware.

**Outbound TLS.** `http_client.cpp` verifies the server chain against the ESP-IDF
CA bundle (`setCACertBundle` + embedded `x509_crt_bundle`), so an active MitM on
the LAN cannot impersonate `opencode.ai` with a self-signed cert and lift the
OAuth tokens. It is the *public* root store, so it also fails closed against a
corporate interception proxy. Costs ~70 KB of flash. There is no `setInsecure()`
left — do not reintroduce it.

Because chain verification also checks the certificate's validity dates the
system clock must be plausible **before the first fetch**. `setup()` calls
`clockSyncRestoreFromRtc()` (the PCF85063 holds the last NTP-synced time, minus
the UTC+8 offset the RTC is written with), and the poll schedule is gated on
`clockSyncValid()` so a blank RTC + blocked NTP shows a clear serial warning
instead of a burst of failed handshakes. If you change the timezone handling in
`clock_sync.cpp`, keep `TZ_OFFSET_SEC` consistent with what `hwRtcWrite()` stores.

**No expiry deadline, but a reflash trigger.** The IDF bundle stores *subject
name + public key* per root (not certificates — `esp_crt_bundle` uses
`mbedtls_pk_parse_public_key` + `mbedtls_pk_verify_ext`), so the anchors have no
validity dates and the device keeps working indefinitely while a host's chain
still terminates at a bundled name with an unchanged key. It breaks only when a
host moves to a CA older than the frozen bundle, or a CA rotates that root's key
— an event, not a date. `tools/check-hosts.sh` detects exactly that by reading
the bundle back out of the built library and checking each host's chain against
it; the bundle's format is `u16 count` then per entry
`u16 name_len, u16 key_len, name(DER), key(DER)`.

Chimes are **synthesised at boot** (`src/voice/chime.*`) into a 500 ms PCM buffer
and streamed from `loop()`; there is no raw file, so activating a chime never
requires an `uploadfs` (which would erase the device's rotated tokens). Chime and
easter-egg playback share the ES8311/PA and are mutually exclusive
(`chimeStop()` before `easterEggStart()`).

`src/gestures.*` is the **only** IMU poll (~33 Hz). It feeds
`gesture::Engine` (`src/util/gesture_util.h`, host-tested) and latches
`EV_SHAKE` / `EV_DOUBLE_TAP` / `EV_PICKUP` / `EV_FACE_DOWN|UP` / `EV_ROT_*`;
`handleGestures()` in `main.cpp` turns them into pager/wake/audio actions.
Thresholds live at the top of `gesture_util.h`. Notes:

- Shake is `|a|² > 4` for three consecutive samples; a double-tap is two jolts
  (above `tapMag2`, below the shake threshold) 80–450 ms apart, so the two never
  collide.
- Pickup compares the low-passed gravity to a slowly-tracked rest reference. The
  pickup test is deliberately **outside** the "quiet" branch — a smooth lift
  keeps `|a| ≈ 1 g` and would otherwise never leave it.
- Face-down is the one case where the panel is **blanked** rather than dimmed
  (`hwDisplayBrightnessRaw(0)`), and touch/key no longer wake it; lifting it back
  restores the level.
- Gestures are suppressed while the easter egg / thinking animation owns the
  screen, and `handleGestures()` ignores taps/rotate in that window.

### Trend page

The Trend page (`src/ui/trend.cpp`) plots the rolling (5-hour) meter as cost:
headline spend, `$/h` burn rate, time-to-limit, a cumulative curve and per-poll
bars. It reads a **RAM-only** ring (`src/usage_history.*`) that `serviceFetch()`
appends to on each successful `FETCH_USAGE`; nothing is persisted, so a reboot
just restarts the history. The pure math lives in `src/util/trend_util.h`
(microcents → `$`, burn rate, ETA) and is host-tested. `UsageData` carries the
`usedMicroCents` / `limitMicroCents` fields the console sends per meter.

### Display power / heat

Rendering is **state-driven, not timer-driven**. `uiFrame()` hashes everything
the frame depends on (`uiFrameKey()` in `src/main.cpp`: page/card, quantised drag
offset, clock HH:MM, online/charging/battery, and a `s_dataRev` counter bumped on
every successful fetch) and re-renders only when that hash actually changes.
Nothing changed ⇒ no render and no panel traffic at all. Most screens hold HH:MM
(not seconds), so a static minute renders exactly once and the idle loop can run
for minutes with zero CPU drawing and zero QSPI.

Each render is a **full-frame push**: `hwDisplayPush()` (`src/hw/display.cpp`)
blits the whole 368×448 window in one QSPI transaction. We deliberately do *not*
track dirty rectangles any more — this CO5300 misbehaves when a frame is split
into many small address windows, and a dirty-rect shadow must stay byte-identical
to the panel or a single bad partial blit desynchronises it permanently (errors
then accumulate as you swipe between pages). The QSPI clock stays at the library
default 40 MHz (`BOARD_DISPLAY_QSPI_HZ`): 80 MHz intermittently wedges the bus
mid-push on this panel, so higher values need to be proven on hardware first.
Idle also dims the panel but **never blanks it** (so it stays glanceable):

- 30 s idle (`IDLE_DIM_MS`) → brightness `BRIGHT_LUT[0]` = 50/255, and it stays
  there until activity (no full screen-off stage).
- Any touch/key (and `state=running` webhooks / shake) wakes it; the waking **tap**
  is swallowed so it doesn't also page — a wake **drag** still scrolls (the swallow
  is dropped as soon as the gesture locks an axis, `pager.cpp`).
- Pager/key/wake/data events set `s_forceRender`, so a swipe/tap never freezes on
  a mid-transition frame.
- Radio: TX power is capped at `WIFI_POWER_11dBm`. Power save is two-phase
  (`hw/net.h`): `APP_WIFI_PS_BOOT` (MIN_MODEM) while associating — MAX_MODEM during
  association can miss the AP's handshake frames and blow past the setup UI's 15 s
  connect grace — then `APP_WIFI_PS` (MAX_MODEM) once connected, for the lowest
  idle radio power at the cost of some status-webhook latency.

Tunables live at the top of `src/main.cpp` (search `IDLE_DIM_MS` / `IDLE_NET_MS`)
and in `data/config.json` (the `poll_*` fields).

### Text rendering (LVGL)

Text is anti-aliased with **LVGL used purely as an offscreen rasteriser**: an
`lv_canvas` points at the existing (PSRAM) Arduino_Canvas framebuffer and
`lv_canvas_draw_text()` blends Montserrat 4bpp glyphs straight into it. No LVGL
widgets, screens or driven display — `src/ui/lvgl_text.*` registers a tiny dummy
display (LVGL needs one before any object exists), creates the canvas once, and
exposes `lvglDrawText()` / `lvglTextWidth()`.

- `src/ui/primitives.cpp` owns the five fonts (`F_BIG..F_SMALL`) and the
  `textAt`/`textRight`/`textCenter` helpers that all card code uses. **The helpers
  take/return logical units and multiply positions by `UI_SCALE` internally** —
  the LVGL fonts are already sized for the canvas, so getting this wrong shifts
  every string by half a screen.
- The fonts are **our own** Montserrat builds in `src/ui/fonts/lv_font_ui_<S>.c`,
  generated with `lv_font_conv` (recipe in `include/lv_conf.h`), *not* LVGL's
  built-ins: those only cover `0x20-0x7F`, `0xB0` and `0x2022`, so the air
  card's `µg/m³` (0xB5 / 0xB3) rendered as garbage. Each generated file is
  guarded by `LV_FONT_UI_<S>`, which must be `1` in `lv_conf.h`.
- Everything else (bars, icons, moon, dots, the 555 logo) is still drawn directly
  with Arduino_GFX into the same buffer; only *text* goes through LVGL.
- `ScaledCanvas` (`hw/display.h`) routes its own `setFont`/`setCursor`/`print`/
  `printf` through LVGL too, so the WiFi setup screen and the shutdown banner
  share the same pipeline. Its cursor is a logical baseline and `print()`
  advances by the rendered width, matching the old u8g2 behaviour.
- `lvglTextInit()` is called at the end of `hwDisplayInit()` (the framebuffer must
  exist first), so LVGL is ready for the `die()` report on any failure after the
  panel is up; before that the canvas is null and `lvglDrawText()` no-ops.
- `-DFRAME_DUMP` + `tools/frame_dump.py` streams each screen and rebuilds it as a
  PNG, which is how the layout is verified without looking at the panel.

### Blocking fetches never freeze the UI

`loop()` is single-threaded and the network fetches are **blocking** (up to
8–12 s each, weather retries ×2), so they run on their own FreeRTOS task —
`src/fetch_task.{h,cpp}`. The previous design ran them inline and pumped the UI
from inside the request (`httpSetUiPump`), but that could only cover the
response-read loop: `client.connect()` (DNS + TCP + TLS handshake) has no
callback to pump from, so the loop froze for seconds. It froze *exactly* on
wake, because `screenWake()` forces a usage fetch the moment a finger lands —
and after a long idle all four pollers are due back to back.

- `loop()` only **schedules** (`fetchTaskStart()`, one job in flight) and
  **harvests** (`serviceFetch()` → `fetchTaskHarvest()`), copying results into
  `s_usage/s_weather/...` and bumping `s_dataRev` on the main task. The fetch
  task never touches UI state, the display or I2C, and runs at the loop task's
  priority so FreeRTOS time-slices input/render against the TLS handshake.
- Jobs carry a `FetchSeed` (the data currently on screen) so a failed poll
  still lands: the fetcher flags weather/moon/air `stale` against real values
  and `serviceFetch()` republishes them without bumping `s_dataRev` — the same
  behaviour as the old in-place mutation.
- The fetch task owns the OAuth `TokenSet` (refresh included). LittleFS is not
  thread-safe and the easter egg / 555 card read files from `loop()`, so
  rotated tokens are handed back via `fetchTaskPendingSave()` and written to
  flash by `serviceFetch()`.
- `handleHttpStatus()` is now called only from `loop()` (every ~16 ms), i.e.
  *more* often than the old 100 ms in-request pump.
- `uiFrame(now)` (`main.cpp`) still owns activity/idle, Key1, the pager, render
  and push, and runs every iteration; the pager also runs while the easter egg
  / `state=running` animation owns the screen.

Each job logs `fetch <name>: ok/fail in <ms>` — on the old inline design that
duration was visible UI freeze, now it is only worker time. The first job also
prints the task's stack headroom (16 KB reserved for mbedTLS).

### Network polling / power

Poll periods live in `data/config.json` (`poll_usage_s`, `poll_usage_idle_s`,
`poll_weather_s`, `poll_air_s`, `poll_moon_s`) and are applied to the `Interval`s
in `setup()`. Defaults: usage 60 s, weather 30 min, air 60 min, moon 12 h.
Two feature toggles live there too: `chime` (completion cues) and
`imu_gestures` (shake / double-tap / pickup / face-down / rotate), both default
`true`. `status_key` is the HMAC secret for the status endpoint (empty =
auth off). Changing `status_key` needs an `uploadfs` — run
`python3 tools/authorize.py` first, because `uploadfs` erases the device's
rotated tokens.

- **Idle backoff.** When the device has been untouched for `IDLE_NET_MS` (30 s),
  the usage poll stretches to `poll_usage_idle_s` (180 s) — the meter only matters
  while someone is looking. Any activity resets it; `screenWake()` also forces one
  usage fetch if the meter has gone >30 s stale (rate-limited so a chatty status
  webhook can't spam fetches).
- **One job in flight.** The `else if` chain picks the most overdue poller and
  hands it to the fetch task; `serviceFetch()` harvests the result before the
  next job is scheduled, so a burst of due timers (all four after a long idle)
  drains one at a time. A timer is only marked run when `fetchTaskStart()`
  accepts the job, so a busy worker just retries a later iteration.

### WiFi scan

`netScan()` (`src/hw/net.cpp`) scans with the station kept up: it aborts any
in-flight connect with a raw `esp_wifi_disconnect()`, turns power save **off**
for the sweep, and uses a 150 ms/channel active scan (~2 s). The old
`WIFI_OFF → WIFI_STA` power-cycle ×3 re-ran WiFi bring-up + RF calibration each
time and, with modem sleep on, stretched a boot scan past 30 s.

On boot with saved credentials the setup UI shows a **30 s connect grace**
(`wifi_setup.cpp`, `CONNECT_GRACE_MS`) before falling back to a scan, so a
slow-but-good connect is no longer aborted by a scan. Tap or press a key to
skip straight to the network list.

### Auth / API notes

- opencode moved the console from the old `/_server` RPC to a REST API under
  `/console/api/*`; the old endpoint now redirects to `/console/login`.
- Requests need `x-org-id: <org id>` (same value as `workspace_id`) plus either an
  `Authorization: Bearer <access token>` or the `__Host-console_session` cookie.
  The firmware uses the Bearer/device-flow path.
- `authorize.py` uses `client_id=opencode-cli`; the device refreshes with the
  `refresh_token` grant and persists rotated tokens to `/tokens.json`.
- Usage meters: `access.meters.{fiveHour,week,month}` with string
  `usedMicroCents` / `limitMicroCents`; percent = used/limit*100.
- Weather: `wttr.in/Beijing?m&format=...` (one-line, ~120 B) — **must** send a
  `curl/...` User-Agent, and HTTP/1.0 avoids chunked encoding.
- Moon: `api.met.no/weatherapi/sunrise/3.0/moon` (~580 B) → moonrise/moonset/phase.
- Air: `api.waqi.info/feed/@3303/?token=<waqi_token>` (@3303 = Beijing US Embassy).
  PM2.5 is sometimes not reported; the card falls back to PM10.

> **⚠️ Throughput note.** Large responses (e.g. wttr.in `format=j1`, ~39 KB) transfer
> at only ~1-3 KB/s on this device, which looks like sender-side pacing. Keep network
> payloads small and prefer the one-line endpoints. `handleHttpStatus()` runs from
> `loop()` every ~16 ms (the fetch task never calls it), and reads drain the TLS
> buffer in an inner loop (calling `available()` per chunk cripples throughput).

### Debug: frame dump

Build with `-DFRAME_DUMP` to have the device walk all 7 screens, stream each raw
RGB565 canvas over USB CDC (`FRAME <i> <w> <h>` + payload) and let a host script
rebuild PNGs — useful for checking layout without looking at the panel:

```bash
PLATFORMIO_BUILD_FLAGS='-DFRAME_DUMP' pio run -e esp32-usage-monitor -t upload
```

## PC-side tools

- `tools/authorize.py` — opencode console device-flow authoriser + `--check`
- `tools/opencode-plugin/` — OpenCode V2 plugin that drives the status endpoint
- `tools/status-ping.sh` — signed status ping, for testing the device by hand
- `tools/status_stub.py` — local stand-in for the status endpoint (tests the plugin + HMAC)
- `tools/check-hosts.sh` — verify the firmware's CA bundle still covers every host it talks to
  (see "Outbound TLS": reads the bundle out of the *built* library, so a CA switch shows up
  here instead of as a silently STALE panel). Exits non-zero when a host needs a firmware update.

## Tests / CI

Firmware flash builds are verified with `pio run`; CI (`.github/workflows/ci.yml`)
runs `pio test -e native` and `pio run -e esp32-usage-monitor` on every push/PR.

The dependency-free helpers under `src/util/` (epoch/date parsing, weather-icon
kind, moon phase, AQI label, `Interval`) and the header-only parsers under
`src/parse/` (usage meters, wttr one-line, moon/air JSON) are covered by host
unit tests — no hardware needed:

```bash
pio test -e native
```

`platformio.ini` keeps the shared ESP32 settings in `[esp32_base]` (extended by
`[env:esp32-usage-monitor]`) so the `[env:native]` test env does not inherit
`framework = arduino`; the native env instead pulls ArduinoJson only (needed by
`src/parse/`) plus `-Isrc -Isrc/util`.
