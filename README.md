# ESP32-S3 Usage Monitor

Firmware for the Waveshare ESP32-S3-Touch-AMOLED-2.16 board that shows OpenCode
token usage, weather and a clock.

## Screens

Three horizontal pages — swipe or tap to switch (Key1 also cycles). The weather
page has five vertical cards; the right-hand dot column marks the current card and
the bottom dot row marks the current page (`@` = active).

```
Weather — Now card
+--------------------------------------------+
| * Beijing                             14:23|
|--------------------------------------------|
|  \o/   24C                               * |
|  /|\   Partly Cloudy                     o |
|                                          o |
|  Feels                               22C o |
|  Humidity                            48% o |
|--------------------------------------------|
|  Wind                            12 km/h NW|
|  Precip                              0.0 mm|
|  Updated 14:20                             |
|                                            |
|                   @ o o                    |
+--------------------------------------------+
```

```
Usage
+--------------------------------------------+
| * Usage                               14:23|
|--------------------------------------------|
|  5-HOUR                                 42%|
|  ##################........................|
|                            resets in 3h 12m|
|  WEEKLY                                 18%|
|  ########..................................|
|                             resets in 2d 4h|
|  MONTHLY                                71%|
|  ###############################...........|
|                             resets in 1d 6h|
|                                            |
|                   o @ o                    |
+--------------------------------------------+
```

```
Clock
+--------------------------------------------+
| * Synced                              14:23|
|--------------------------------------------|
|                                            |
|                   14:23                    |
|               2026-10-08 Thu               |
|                                            |
|--------------------------------------------|
|  Battery 86%  charging                     |
|  #####################################.....|
|                                            |
|                  [ 555 ]                   |
|                   o o @                    |
+--------------------------------------------+
```

## Features

- **Three horizontal pages**: Weather ↔ Usage ↔ Clock, swipe or tap to switch
- **Vertical weather cards**: Now → Sun → Air → Moon → Moonrise/Moonset
- Usage shows **5-hour** / **week** / **month** progress bars
- Countdown to the next reset (days / hours / minutes)
- Bars turn yellow above 30% and red above 60%
- Auto refresh: usage every 60 s, weather every 15 min, moon every 6 h, air quality every 30 min
- Status bar with a connection dot, page title and time (battery and charging state are on the Clock page)
- **OAuth device flow**: authorise once with `tools/authorize.py`; the refresh token rotates automatically and is persisted to LittleFS
- WiFi setup UI (scan + on-screen keyboard / button password entry)
- Shake the device to play the 555 easter egg
- Configuration stored in the LittleFS filesystem

## Hardware

- **Board**: Waveshare ESP32-S3-Touch-AMOLED-2.16
- **Display**: 480×480 AMOLED (CO5300 driver)
- **Touch**: CST9220
- **PSRAM**: 8 MB | **Flash**: 16 MB (8 MB app partitions)

## Setup

1. Install [PlatformIO](https://platformio.org/)
2. Clone the repository:
   ```bash
   git clone https://github.com/2700zc/esp32-usage-monitor.git
   cd esp32-usage-monitor
   ```

## Configuration

Copy the template:

```bash
cp data/config.json.example data/config.json
```

Fields in `data/config.json`:

```json
{
  "workspace_id": "wrk_XXXXXXXXXXXXXXXXXXXXXXXX",
  "refresh_token": "",
  "org_id": "",
  "waqi_token": ""
}
```

| Field | Required | Description |
|---|---|---|
| `workspace_id` | ✅ | Your workspace / org id (starts with `wrk_`) |
| `refresh_token` |  | Written automatically by `tools/authorize.py`; do not fill in by hand |
| `org_id` |  | Written automatically by the authoriser; falls back to `workspace_id` when empty |
| `waqi_token` |  | Optional [aqicn.org](https://aqicn.org/data-platform/token/) token, enables the air-quality card |

### Authorisation (OAuth device flow)

```bash
python3 tools/authorize.py
```

Open the printed URL, sign in and confirm the device code. The script writes
`refresh_token` / `org_id` back into `data/config.json`. Then upload the filesystem:

```bash
pio run -e esp32-usage-monitor -t uploadfs
```

> **⚠️ Token lifecycle**
> The refresh token is **single-use** and rotates on every refresh; reusing an old
> one revokes the whole token family (and you must re-authorise). After the first
> boot the device takes ownership and saves the rotated token to LittleFS
> `/tokens.json`.
>
> - `pio run -t upload` (firmware only) is **safe** — it does not touch the filesystem.
> - `pio run -t uploadfs` **erases** LittleFS (including `/tokens.json`), so the device
>   falls back to the possibly stale token in `config.json`. **Always run
>   `python3 tools/authorize.py` right before an `uploadfs`.**
> - Do **not** run `python3 tools/authorize.py --check` while the device is live — it
>   rotates the token and invalidates the device's copy.

## Build & upload

```bash
# compile
pio run -e esp32-usage-monitor

# upload firmware
pio run -e esp32-usage-monitor -t upload

# upload filesystem (includes config.json)
pio run -e esp32-usage-monitor -t uploadfs

# upload both
pio run -e esp32-usage-monitor -t upload -t uploadfs
```

OTA (when the device is already on WiFi):

```bash
pio run -t upload --upload-port <ESP32_IP>
```

### macOS arm64 note

PlatformIO's `tool-mklittlefs` package ships an x86_64-only binary, so `uploadfs`
fails with `mklittlefs: Bad CPU type in executable`. Download the
`aarch64-apple-darwin-*.tar.gz` asset from
<https://github.com/jason2866/mklittlefs/releases> and replace
`~/.platformio/packages/tool-mklittlefs@3.2.0/mklittlefs`.

## Usage

1. On first boot the device opens the WiFi setup UI. Scan, pick a network and enter
   the password:
   - **Button A (left)**: short press cycles the character, long press (600 ms) switches the character group (abc/ABC/123/symbols)
   - **Button B (middle)**: short press types the character, long press (800 ms) connects
   - The on-screen touch keyboard works too
2. Once connected the device enters the desktop automatically and syncs NTP
3. Swipe left/right (or tap) to switch between **Weather / Usage / Clock**; swipe up/down on the weather page to switch cards
4. Press the leftmost button (Key1) to cycle pages
5. Long-press the power key to shut down

## Project structure

```
├── data/
│   ├── config.json          # runtime config (gitignored)
│   ├── config.json.example  # config template
│   ├── 555_audio.raw        # shake easter-egg audio
│   └── 555.raw              # shake easter-egg image
├── src/
│   ├── main.cpp             # setup/loop, network polling, render dispatch
│   ├── pager.cpp/h          # page + weather-card gesture/animation machine
│   ├── status_server.cpp/h  # HTTP status server (?state=running|done|failed)
│   ├── clock_sync.cpp/h     # NTP bootstrap + RTC resync scheduling
│   ├── easter_egg.cpp/h     # shake detection + 555 audio playback
│   ├── storage.cpp/h        # LittleFS mount helper (one-shot, format-on-fail)
│   ├── config.h             # config struct + LittleFS JSON loader
│   ├── token_store.cpp/h    # OAuth tokens persisted to LittleFS
│   ├── http_client.cpp/h    # shared blocking HTTPS helper
│   ├── api_client.cpp/h     # OpenCode console REST client
│   ├── weather_client.cpp/h # weather / moon / air quality
│   ├── usage_display.cpp/h  # UI drawing (pages, cards, icons)
│   ├── wifi_setup.cpp/h     # WiFi setup UI
│   ├── util/                # dependency-free helpers (host-unit-tested)
│   │   ├── time_util.h      # epoch / ISO / HTTP-date parsing
│   │   └── value_util.h     # weather-icon kind, moon phase, AQI label
│   ├── voice/               # I2S audio (ES8311, easter-egg playback)
│   ├── boards/              # per-board pin definitions
│   └── hw/                  # hardware driver layer
│       ├── display.cpp/h    # AMOLED display driver + touch coord mapping
│       ├── input.cpp/h      # button and touch input
│       ├── power.cpp/h      # power management (AXP2101)
│       ├── imu.cpp/h        # accelerometer
│       ├── rtc.cpp/h        # real-time clock
│       ├── expander.cpp/h   # IO expander
│       ├── net.cpp/h        # WiFi connection and credentials
│       ├── pins.h           # pin definitions
│       └── hw.cpp/h         # hardware init
├── test/
│   └── test_util/           # host unit tests (pio test -e native)
├── tools/
│   └── authorize.py         # OAuth device-flow authoriser / --check
└── platformio.ini           # PlatformIO config
```

## Tests

The pure helpers under `src/util/` (epoch/date parsing, weather-icon kind,
moon phase, AQI label) are free of Arduino dependencies and covered by host
unit tests — no hardware needed:

```bash
pio test -e native
```

## Notes

- `data/config.json` contains credentials and is excluded by `.gitignore`; it is never committed
- After editing `config.json`, re-upload the filesystem: `pio run -e esp32-usage-monitor -t uploadfs`
- Tokens rotate on the device; to re-authorise, run `python3 tools/authorize.py` and then `uploadfs`
- Weather comes from [wttr.in](https://wttr.in/Beijing) (Beijing), moon data from MET Norway, air quality from waqi

## Related
- A Windows floating usage widget is also available: https://github.com/2700zc/TokenHub — ready to use once configured

## License

MIT
