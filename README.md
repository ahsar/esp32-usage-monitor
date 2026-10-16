# ESP32-S3 Usage Monitor

A desk display for [OpenCode](https://opencode.ai) usage on a Waveshare
ESP32-S3-Touch-AMOLED-2.16: your rolling token spend, a trend view, the weather
and a clock — and the panel reacts while the agent is working.

<p align="center">
  <img src="docs/screens/usage.png" width="300" alt="Usage page with the 5-hour, weekly and monthly meters">
</p>

Four pages (Weather · Usage · Trend · Clock) driven by touch or key, five
weather cards, an OpenCode status bridge with chimes, and a Trend page that plots
spend rate and time-to-limit.

## Screens

<p align="center">
  <img src="docs/screens/weather-now.png" width="200" alt="Weather page, Now card">
  <img src="docs/screens/usage.png" width="200" alt="Usage page">
  <img src="docs/screens/trend.png" width="200" alt="Trend page">
  <img src="docs/screens/clock.png" width="200" alt="Clock page">
  <br>
  <sub><b>Weather</b> (Now card) &nbsp;·&nbsp; <b>Usage</b> &nbsp;·&nbsp; <b>Trend</b> &nbsp;·&nbsp; <b>Clock</b></sub>
</p>

The Weather page holds five vertical cards — swipe up/down to move between them:

<p align="center">
  <img src="docs/screens/weather-sun.png" width="200" alt="Weather card: Sun">
  <img src="docs/screens/weather-air.png" width="200" alt="Weather card: Air">
  <img src="docs/screens/weather-moon.png" width="200" alt="Weather card: Moon">
  <img src="docs/screens/weather-moonset.png" width="200" alt="Weather card: Moonrise and Moonset">
  <br>
  <sub><b>Sun</b> &nbsp;·&nbsp; <b>Air</b> &nbsp;·&nbsp; <b>Moon</b> &nbsp;·&nbsp; <b>Moonrise</b></sub>
</p>

The bottom dot row marks the current page and the right-hand dot column the
current weather card. These are real captures from the device — see
[Notes](#notes) for how to regenerate them.

## Features

- **Four horizontal pages** — Weather ↔ Usage ↔ Trend ↔ Clock: swipe, tap or
  press Key1.
- **Five weather cards** — Now → Sun → Air → Moon → Moonrise/Moonset.
- **Usage meters** — 5-hour rolling / weekly / monthly, each with a reset
  countdown; the bars go green, yellow above 30% and red above 60%.
- **Trend page** — 5-hour rolling spend, `$/h` burn rate, estimated time to
  limit, plus a cumulative spend curve and per-poll bars. The history ring is
  RAM-only, so it starts empty on every boot and fills as polls land.
- **Status bridge** — an OpenCode plugin drives the device over HTTP: a
  "Thinking…" animation while the agent works, "Needs you" plus an attention
  beep when it asks for permission, and a two-beep / low-beep cue when a session
  succeeds or fails.
- **Chimes** are synthesised at boot on the ES8311 codec — nothing to upload. Soft
  struck notes rather than beeps, with a loudness (`volume`) you can set live.
- **Remote settings** — chime (and its volume), brightness and the whole location (place,
  coordinates, timezone, air station) are changeable over the status endpoint
  with `tools/status-settings.sh`, so tuning the device never means another
  `uploadfs`.
- **OAuth device flow** — authorise once with `tools/authorize.py`; the refresh
  token rotates automatically and is persisted to LittleFS.
- **WiFi setup UI** on first boot — scan, pick a network, type the password with
  the touch keyboard or the two keys.
- **Glanceable power** — 30 s without input dims the panel to 50/255 and any
  touch, key or status event wakes it. The panel is never blanked, and a frame is
  re-rendered and pushed only when something on it actually changes.

## Hardware

- **Board**: Waveshare ESP32-S3-Touch-AMOLED-2.16 — ESP32-S3, 8 MB PSRAM,
  16 MB flash (an 8 MB partition table: two 3 MB OTA app slots + ~2 MB LittleFS)
- **Display**: 480×480 rounded-square AMOLED (CO5300, QSPI). The UI is authored
  on a 184×224 logical grid, rasterised at 2× and blitted as a 368×448 canvas
  centred in the panel
- **Touch**: CST9220 · **RTC**: PCF85063 · **PMU**: AXP2101
- **Audio**: ES8311 codec + PA (chimes)
- **Keys**: Key1 (middle, PWR), Key2 (left, IO18), BOOT (right)

The current build targets the 2.16 board (`-DBOARD_WAVESHARE_ESP32S3_TOUCH_AMOLED_2_16`
in `platformio.ini`); pin maps for the ESP32-S3 AMOLED 1.8 and 1.75c and the
ESP32-C6 AMOLED 2.16 live in `src/boards/`, though only the 2.16 is wired into
the build today.

## Quick start

1. Install [PlatformIO](https://platformio.org/) and clone the repository:

   ```bash
   git clone https://github.com/2700zc/esp32-usage-monitor.git
   cd esp32-usage-monitor
   ```

2. Create your config and fill in `workspace_id`:

   ```bash
   cp data/config.json.example data/config.json
   ```

3. Authorise the device (OAuth device flow) and flash it:

   ```bash
   python3 tools/authorize.py
   pio run -e esp32-usage-monitor -t upload -t uploadfs
   ```

4. On first boot the device opens the WiFi setup UI; after that it goes straight
   to the desktop and syncs the clock over NTP.

### Configuration

`data/config.json` (gitignored — it holds credentials):

```json
{
  "workspace_id": "wrk_XXXXXXXXXXXXXXXXXXXXXXXX",
  "refresh_token": "",
  "org_id": "",
  "waqi_token": "",
  "status_key": "",
  "ota_password": "",

  "place": "Beijing",
  "lat": 39.9042,
  "lon": 116.4074,
  "tz_offset_min": 480,
  "waqi_station": "@3303",
  "air_label": "",

  "poll_usage_s": 60,
  "poll_usage_idle_s": 180,
  "poll_weather_s": 1800,
  "poll_air_s": 3600,
  "poll_moon_s": 43200,

  "chime": true
}
```

| Field | Default | Description |
|---|---|---|
| `workspace_id` | — | **Required.** Your workspace / org id (starts with `wrk_`) |
| `refresh_token` | — | Written by `tools/authorize.py`; do not fill in by hand |
| `org_id` | — | Written by the authoriser; falls back to `workspace_id` when empty |
| `waqi_token` | — | Optional [aqicn.org](https://aqicn.org/data-platform/token/) token; enables the Air card |
| `status_key` | — | Shared secret for the status endpoint (HMAC-SHA256). **Empty disables its auth** |
| `ota_password` | — | **Required (≥ 12 chars)** for the wireless update endpoint; empty/short disables OTA |
| `place` | `Beijing` | City shown in the status bar and queried on wttr.in (spaces are URL-encoded) |
| `lat` / `lon` | `39.9042` / `116.4074` | Coordinates the moonrise/moonset are computed for (MET Norway) |
| `tz_offset_min` | `480` | Minutes east of UTC (`480` = UTC+8, `-300` = UTC−5). Drives NTP and the RTC's wall time |
| `waqi_station` | `@3303` | aqicn.org station id the Air card reads |
| `air_label` | `place` | Air card subtitle (the monitoring station); empty = use `place` |
| `poll_usage_s` | `60` | Usage poll period (seconds) |
| `poll_usage_idle_s` | `180` | Usage poll period while the device has been untouched for 30 s |
| `poll_weather_s` | `1800` | Weather poll period |
| `poll_air_s` | `3600` | Air-quality poll period |
| `poll_moon_s` | `43200` | Moon poll period |
| `chime` | `true` | Play a chime when the agent finishes or needs you |
| `ble_status` | `true` | Also serve the status/settings channel over BLE, as a fallback for a host the LAN cannot reach |

The location block is optional: leave it out and the device keeps the defaults
above. Changing `tz_offset_min` needs a note of its own — the PCF85063 stores
*local* wall time, so the first boot after the change reads the old offset's time
with the new offset and can be off by the difference until NTP answers (the
firmware retries every 5 s until it does, then hourly).

Most of these can also be changed without touching this file at all: see
[Tuning it over HTTP](#tuning-it-over-http), which writes them to
`/settings.json` on the device (layered over this file) instead.

### Authorisation (OAuth device flow)

```bash
python3 tools/authorize.py          # open the printed URL, sign in, confirm the code
python3 tools/authorize.py --check  # refresh and print the current usage (device must be off)
```

The script writes `refresh_token` / `org_id` back into `data/config.json`.

> **⚠️ Token lifecycle**
> The refresh token is **single-use** and rotates on every refresh; reusing an old
> one revokes the whole token family (and you must re-authorise). After the first
> boot the device takes ownership and saves the rotated token to LittleFS
> `/tokens.json`.
>
> - `pio run -t upload` (firmware only) is **safe** — it does not touch the filesystem.
> - `pio run -t uploadfs` **erases** LittleFS (including `/tokens.json`), so the device
>   falls back to the possibly stale token in `config.json`. **Always run
>   `python3 tools/authorize.py` right before an `uploadfs`.** A second `uploadfs`
>   of an unchanged config is the case that breaks, so the target refuses to run
>   while `config.json` still holds the token it already pushed
>   (`tools/uploadfs_guard.py`); override with `ESP32_ALLOW_STALE_TOKEN=1` if you
>   really mean it.
> - Do **not** run `python3 tools/authorize.py --check` while the device is live — it
>   rotates the token and invalidates the device's copy.

## Build & upload

```bash
# compile
pio run -e esp32-usage-monitor

# upload firmware
pio run -e esp32-usage-monitor -t upload

# upload filesystem (config.json)
pio run -e esp32-usage-monitor -t uploadfs

# upload both
pio run -e esp32-usage-monitor -t upload -t uploadfs
```

### Wireless update (OTA)

The partition table has two 3 MB app slots (see [Hardware](#hardware)), so
firmware can be pushed over WiFi once the device is running this build. Only the
*app* is replaced — LittleFS is untouched, so an OTA keeps `config.json` and the
rotated OAuth tokens (no `authorize.py`, no re-auth).

```bash
# enable the WiFi path once over USB (no filesystem change → tokens survive)
pio run -e esp32-usage-monitor -t upload

# from then on, over the air
tools/ota.sh 192.168.1.42
```

`tools/ota.sh` reads `ota_password` from `data/config.json`, hands it to espota
and retries (the transfer is link-bound). `pio run -e esp32-ota -t upload
--upload-port <ip>` works too, but then espota needs the same secret in the
`OTA_AUTH` environment variable. The default env keeps using USB on purpose. The
IP is in the boot log (`diag: ip=…`) and the device also answers to
`usage-monitor.local`. While a transfer runs the panel shows `Updating …%` and
polling is paused; on success the device reboots into the new slot by itself.

The endpoint is **password-only**: `ota_password` must be at least 12 characters
or the device does not start the OTA server at all (`ota: DISABLED …` in the boot
log). A generated secret is 32 characters:

```bash
tools/ota.sh --gen-password   # writes data/config.json, then uploadfs as printed
```

The first `-e esp32-ota` run compiles into its own build directory
(`.pio/build/esp32-ota`), so it takes a full build before the upload starts — not
a hang. The transfer itself is the slow part: espota acks every 1 KB chunk, so
Nagle + delayed-ACK caps it around 20 KB/s on a quiet link (~80 s) and packet
loss drags it down much further (measured: 5 KB/s and a 5-minute transfer over a
link losing 24% of its packets). A failed transfer is safe (see below) — just run
the command again.

Only the app can go over the air: the filesystem and the partition table still
need USB (`-t uploadfs` cannot be combined with `esp32-ota`).

espota takes the password as a command-line argument, so it is briefly visible in
the local process list — a detail of PlatformIO's espota integration, not of the
device.

A failed transfer changes nothing — the running app stays in flash and polling
resumes (the log says `ota: FAILED (…)`). There is no automatic rollback, so an
image that crashes on boot has to be recovered over USB.

### macOS arm64 note

PlatformIO's `tool-mklittlefs` package ships an x86_64-only binary, so `uploadfs`
fails with `mklittlefs: Bad CPU type in executable`. Download the
`aarch64-apple-darwin-*.tar.gz` asset from
<https://github.com/jason2866/mklittlefs/releases> and replace
`~/.platformio/packages/tool-mklittlefs@3.2.0/mklittlefs`.

## Using it

- **WiFi setup (first boot).** The device scans and lists networks; tap one to
  select it, or use the keys. With saved credentials it first gives the connect a
  30 s grace period — tap or press a key to skip straight to the network list.
  - **Key1 (middle)**: short press cycles the character, long press (600 ms)
    switches the character group (`abc` / `ABC` / `123` / symbols)
  - **Key2 (left)**: short press types the character, long press (800 ms)
    connects
  - The on-screen touch keyboard does the same thing directly
- **Pages.** Swipe left/right (or tap) to move between Weather / Usage / Trend /
  Clock. On the Weather page, swipe up/down for the five cards.
- **Key1** cycles pages, and dismisses the "Thinking…" / "Needs you" animation.
- **Power key** (middle, the AXP `PWRON` input): tap to power on, hold ~2.5 s to
  shut down. The press that switched the device on is ignored, so holding the key
  a moment too long will not power it straight back off.

## Status bridge (OpenCode → device)

The device runs a small HTTP server on port 80 (advertised over mDNS as
`usage-monitor.local`); `tools/opencode-plugin/` is its client and pushes the
agent's state:

| OpenCode V2 event | Request | Device |
|---|---|---|
| `session.execution.started` | `?state=running` | "Thinking…" animation, wakes the panel |
| `permission.asked` | `?state=waiting` | "Needs you" animation + attention beep |
| `session.execution.succeeded` / `interrupted` | `?state=done` | rising two-beep cue |
| `session.execution.failed` | `?state=failed` | low beep |

Every request is signed with `HMAC-SHA256(status_key, "<state>|<unixSeconds>")`
in `X-Sig` / `X-Ts`; a bad or missing signature gets `401` and changes nothing,
and a ±60 s window blocks replays. See
[`tools/opencode-plugin/README.md`](tools/opencode-plugin/README.md) for setup,
and `tools/status-ping.sh` to drive the endpoint by hand.

The same channel is also served over BLE, for a host the LAN cannot reach (VLAN
boundaries, a network that blocks mDNS, an AP that carries nothing). The plugin
tries the LAN first and falls back to it; `tools/ble-bridge.py` is the PC end. It
needs **no pairing** — the HMAC is what authorises a request, not the link — and no
configuration. See [docs/pc-bridge.md](docs/pc-bridge.md#the-same-channel-over-ble-when-the-lan-cannot-carry-it).

> BLE only carries what the host initiates (state + settings). The device's own
> usage/weather/moon/air fetches go to the internet and still need WiFi.

### Tuning it over HTTP

The same server carries a small settings channel, so the knobs you would
otherwise edit into `data/config.json` — and `uploadfs`, which erases the device's
rotated OAuth token — can be changed from the couch instead:

```bash
tools/status-settings.sh                     # what is overridden right now
tools/status-settings.sh chime=0 brightness=3
tools/status-settings.sh place=Shanghai lat=31.2304 lon=121.4737
tools/status-settings.sh place=              # clear it: back to config.json
```

| Key | Value |
|---|---|
| `chime` | `0` / `1` |
| `volume` | `0`–`5` chime loudness (the default is `3`; `chime=0` is off, not `volume=0`) |
| `brightness` | `1`–`4` (the awake panel level; `0` is the idle-dim level) |
| `place` | city for wttr.in and the status bar |
| `lat`, `lon` | moonrise/moonset coordinates (set together) |
| `tz_offset_min` | minutes east of UTC (resyncs NTP + the RTC) |
| `waqi_station` | aqicn.org station id |
| `air_label` | Air card subtitle |

They are stored in `/settings.json` on the device and layered over `config.json`,
so a reboot keeps them and clearing one restores the configured value; the same
signature as the state pings applies (over the raw query string), and with no
`status_key` the device refuses settings outright rather than accepting an
unauthenticated write. See [docs/pc-bridge.md](docs/pc-bridge.md#settings-over-the-same-endpoint).

## Project layout

```
├── data/
│   ├── config.json          # runtime config (gitignored)
│   └── config.json.example  # config template
├── src/
│   ├── main.cpp             # setup/loop, UI frame gate, idle/dim, wiring
│   ├── poller.cpp/h         # poll schedule + backoff + wedge ladder (tested)
│   ├── data_types.h         # shared payload structs + FetchJob
│   ├── ui_compose.cpp/h     # picks the page/cards to draw for the pager state
│   ├── ui_model.h           # neutral UiPage / UiWeatherCard enums + UiEnv
│   ├── pager.cpp/h          # page/card gesture + animation state machine
│   ├── status_server.cpp/h  # HTTP status endpoint (signed state + settings)
│   ├── clock_sync.cpp/h     # NTP bootstrap + RTC resync scheduling
│   ├── usage_history.cpp/h  # RAM ring of spend samples (Trend page)
│   ├── storage.cpp/h        # LittleFS mount helper
│   ├── config.h             # AppConfig + LittleFS JSON loader
│   ├── token_store.cpp/h    # OAuth tokens persisted to LittleFS
│   ├── settings_store.cpp/h # runtime overrides persisted to /settings.json
│   ├── http_client.cpp/h    # shared blocking HTTPS helper
│   ├── fetch_task.cpp/h     # background fetch worker (job/result queues)
│   ├── api_client.cpp       # OpenCode console REST client
│   ├── weather_client.cpp/h # weather / moon / air quality
│   ├── wifi_setup.cpp/h     # WiFi setup UI
│   ├── frame_dump.cpp/h     # -DFRAME_DUMP screen walker (screenshot tool)
│   ├── diag.cpp/h           # boot/fetch freeze diagnostics
│   ├── usage_display.h      # public UI drawing API
│   ├── ui/                  # cards, trend, primitives, icons, lvgl_text, theme
│   ├── parse/               # header-only parsers (usage/weather/moon/air/settings)
│   ├── util/                # dependency-free helpers (host-unit-tested)
│   ├── voice/               # I2S audio (ES8311, chimes)
│   ├── boards/              # per-board pin definitions
│   └── hw/                  # display, input, power, rtc, expander, net
├── fonts/                   # own Montserrat builds (lv_font_ui_*.c, generated)
├── docs/                    # subsystem deep dives (linked from AGENTS.md)
├── test/                    # host unit tests (pio test -e native)
├── tools/                   # PC-side scripts + the OpenCode plugin
└── platformio.ini           # PlatformIO config
```

## Tests

The pure helpers under `src/util/` (epoch/date parsing, weather-icon kind, moon
phase, AQI label, trend math, `Interval`), the header-only
parsers under `src/parse/` (usage meters, wttr one-line, moon/air JSON) and the
poll scheduler in `src/poller.cpp` are free of Arduino and hardware dependencies
and covered by host unit tests — no hardware needed:

```bash
pio test -e native
```

CI runs that plus `pio run -e esp32-usage-monitor` on every push and PR.

## Notes

- `data/config.json` holds credentials and is gitignored; never commit it. After
  editing it, re-upload the filesystem (`-t uploadfs`) — but run
  `python3 tools/authorize.py` first, because that erase takes the device's
  rotated token with it. `tools/uploadfs_guard.py` stops the `uploadfs` if you
  forget.
- The screenshots above come from the device itself. Build with
  `PLATFORMIO_BUILD_FLAGS='-DFRAME_DUMP' pio run -e esp32-usage-monitor -t upload`,
  then `python3 tools/frame_dump.py /dev/cu.usbmodemXXXX docs/screens` walks the
  pages and rebuilds each raw RGB565 frame as a PNG.
- Sources: weather from [wttr.in](https://wttr.in/Beijing) (the config `place`,
  Beijing by default), moon data from MET Norway (config `lat`/`lon`), air quality
  from [waqi](https://aqicn.org) (config `waqi_station`, the US Embassy monitoring
  station in Beijing by default).
- The design notes — token lifecycle, the fetch worker, display power, TLS trust
  anchors, text rendering — live in [`AGENTS.md`](AGENTS.md); the change history
  is in [`CHANGELOG.md`](CHANGELOG.md).

## License

MIT
