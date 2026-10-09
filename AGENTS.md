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
| `src/main.cpp` | Entrypoint: setup/loop, network polling, render dispatch |
| `src/pager.{h,cpp}` | Two-axis page/weather-card gesture + animation state machine |
| `src/status_server.{h,cpp}` | HTTP status server (`?state=running\|done\|failed`) |
| `src/clock_sync.{h,cpp}` | NTP bootstrap + RTC resync scheduling |
| `src/easter_egg.{h,cpp}` | Shake detection + 555 audio playback |
| `src/storage.{h,cpp}` | LittleFS mount helper (one-shot, format-on-fail) |
| `src/config.h` | `AppConfig` struct + LittleFS JSON loader (`/config.json`) |
| `src/token_store.{h,cpp}` | Runtime OAuth tokens persisted to LittleFS (`/tokens.json`) |
| `src/http_client.{h,cpp}` | Shared blocking HTTPS helper (headers, HTTP/1.0, timeouts) |
| `src/api_client.cpp` | opencode console REST client (`https://opencode.ai/console/api/go/status`) |
| `src/weather_client.{h,cpp}` | Weather (wttr.in), moon (MET Norway), air quality (waqi) |
| `src/usage_display.{h,cpp}` | UI: page/card renderers, icons, easter egg |
| `src/util/` | Dependency-free helpers (time/value parsing, host-unit-tested) |
| `src/wifi_setup.cpp` | WiFi config UI (button-based password entry) |
| `src/hw/` | Drivers: display, input, power, IMU, RTC, net |
| `src/boards/` | Board-specific pin definitions |
| `src/voice/` | I2S audio output (ES8311) |
| `test/` | Host unit tests (`pio test -e native`) |
| `tools/` | PC-side Python tools |

### UI / navigation

- Horizontal pages: **Weather ↔ Usage ↔ Clock**.
- Weather has vertical cards: **Now → Sun → Air → Moon → Moonrise**.
- Drag follows the finger and snaps; tap advances; Key1 cycles. Dots: bottom row =
  pages, right column = weather cards.
- All on-screen copy is English. Only Latin fonts are used (LOGISOSO/helvB), so the
  CJK point fonts were dropped (saves ~280 KB flash).
- Weather cards reserve the right `WC_R` margin so content never collides with the
  card dots.

### Display power / heat

The panel is only pushed when the canvas is dirty (`s_dirty`), so idle frames drop
from ~60/s to ~1/s instead of re-sending the full 480×480 frame every loop. Idle
also dims the panel but **never blanks it** (so it stays glanceable):

- 30 s idle (`IDLE_DIM_MS`) → brightness `BRIGHT_LUT[0]` = 50/255, and it stays
  there until activity (no full screen-off stage).
- Any touch/key (and `state=running` webhooks / shake) wakes it; the waking tap is
  swallowed so it doesn't also page.
- Pager state changes (`s_uiDirty`) force an immediate redraw, so a swipe/tap never
  freezes on a mid-transition frame while waiting for the 1 s idle tick.
- Radio: modem sleep is on (`WIFI_PS_MIN_MODEM`) and TX power is capped at
  `WIFI_POWER_11dBm` — both cut heat a lot. Raise the TX power if WiFi is weak.

Tunables live at the top of `src/main.cpp` (search `IDLE_DIM_MS`).

### WiFi scan

`netScan()` (`src/hw/net.cpp`) scans with the station kept up: it aborts any
in-flight connect with a raw `esp_wifi_disconnect()`, turns power save **off**
for the sweep, and uses a 150 ms/channel active scan (~2 s). The old
`WIFI_OFF → WIFI_STA` power-cycle ×3 re-ran WiFi bring-up + RF calibration each
time and, with modem sleep on, stretched a boot scan past 30 s.

On boot with saved credentials the setup UI shows a **15 s connect grace**
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
> payloads small and prefer the one-line endpoints. `handleHttpStatus()` is pumped
> every 100 ms during a fetch, and reads drain the TLS buffer in an inner loop
> (calling `available()` per chunk cripples throughput).

### Debug: frame dump

Build with `-DFRAME_DUMP` to have the device walk all 7 screens, stream each raw
RGB565 canvas over USB CDC (`FRAME <i> <w> <h>` + payload) and let a host script
rebuild PNGs — useful for checking layout without looking at the panel:

```bash
PLATFORMIO_BUILD_FLAGS='-DFRAME_DUMP' pio run -e esp32-usage-monitor -t upload
```

## PC-side tools

- `tools/authorize.py` — opencode console device-flow authoriser + `--check`  

## Tests / no CI

Bare-metal firmware, no CI pipeline. Verify flash builds with `pio run`.

The dependency-free helpers under `src/util/` (epoch/date parsing, weather-icon
kind, moon phase, AQI label) are covered by host unit tests — no hardware
needed:

```bash
pio test -e native
```

`platformio.ini` keeps the shared ESP32 settings in `[esp32_base]` (extended by
`[env:esp32-usage-monitor]`) so the `[env:native]` test env does not inherit
`framework = arduino`/`lib_deps`. `default_envs` keeps bare `pio run` to the
firmware only.
