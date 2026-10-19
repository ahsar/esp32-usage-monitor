# PC bridge and chimes

The device never talks to OpenCode; OpenCode talks to the device. `status_server`
is a plain HTTP server on port 80 and `tools/opencode-plugin/` (OpenCode **V2**
plugin, `session.execution.*` / `permission.asked` events) is the client. The same
channel is also served over BLE for hosts the LAN cannot reach — see
[the BLE transport](#the-same-channel-over-ble-when-the-lan-cannot-carry-it) below.

| Request | State | Device |
|---|---|---|
| `?state=running` | agent working | "Thinking…" animation, wakes the panel |
| `?state=waiting` | permission/question | "Needs you" animation + attention beep |
| `?state=done` / `?state=failed` | finished / errored | rising two-beep / low beep |

The same channel also carries the settings channel and the WiFi credentials
request — see below.

`done`/`failed` only chime on a real transition, and never for the Key1 dismiss,
so hand-dismissing the animation stays silent. Do **not** rely on V1 event names
(`session.idle`, `session.status`, `permission.updated`) — V2 does not emit them.
`tools/status-ping.sh` drives the same endpoint by hand, so the firmware path can
be tested without OpenCode. The device advertises `usage-monitor.local` over
mDNS (`ESPmDNS`) so the plugin needs no DHCP IP — and where mDNS does not cross a
segment (an office VLAN), the ladder in `tools/monitor-send.sh` carries the device's
IP instead.

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

**Chimes.** Synthesised at boot (`src/voice/chime.*`) into a 700 ms PCM buffer
and streamed from `loop()`; there is no raw file, so activating a chime never
requires an `uploadfs` (which would erase the device's rotated tokens). The three
cues are struck notes (fast attack, exponential decay, a touch of 2nd/3rd
harmonic) — a rising 660→990 Hz pair on success, one long low 330 Hz note on
failure and two soft 880 Hz taps when you are needed. Their loudness is the
`?volume=` setting (0..5, default 3), applied on the next cue.

## Settings over the same endpoint

The same port carries a second, small protocol: the settings channel. It exists
because the alternative for every knob below is editing `data/config.json` and
running `uploadfs`, which **erases the device's rotated OAuth token** (see
`tools/uploadfs_guard.py`) — far too much ceremony for "make the chimes stop".

| Request | Meaning |
|---|---|
| `?chime=0` / `?chime=1` | chime on completion / needs-you |
| `?volume=0..5` | chime loudness (0 is the quietest step, not silence; default 3) |
| `?brightness=1..4` | awake panel level (index into `BRIGHT_LUT`; 0 is the dim level, so it is not settable) |
| `?place=CITY` | wttr.in query and status-bar title |
| `?lat=..&lon=..` | moonrise/moonset coordinates (must be given together) |
| `?tz_offset_min=MIN` | minutes east of UTC; re-syncs NTP and rewrites the RTC in the new zone |
| `?waqi_station=@ID` | aqicn.org station the Air card reads |
| `?air_label=LABEL` | Air card subtitle (empty = the place) |
| `?settings` | read the current overrides back (no changes) |
| `key=` (empty value) | clear that override, so config.json's value applies again |

`tools/status-settings.sh` is the client; it signs for you:

```bash
tools/status-settings.sh                              # show what is overridden
tools/status-settings.sh chime=0 brightness=3
tools/status-settings.sh place=Shanghai lat=31.2304 lon=121.4737
tools/status-settings.sh place=                     # back to config.json
```

**Signing.** A settings request is signed over the **raw query string** —
`"chime=0&brightness=3|<ts>"` — not over a single value, so every field in the
request is bound to the key (a captured request cannot be edited into a different
one inside the timestamp window) and the client needs no canonicalisation: it
hashes exactly the bytes it puts in the URL. `?state=` keeps the original
value-only signature, so the plugin is untouched by any of this. Values are
URL-decoded by the device, so a space is `+` or `%20`.

**Validation and storage.** `src/parse/settings_parse.h` is the whole decision —
key names, ranges, "lat and lon belong together", clearing — and it is
host-tested; a bad key, an out-of-range number or a half-specified location
returns `400` with a message and **nothing** is applied (all-or-nothing, so a
typo cannot half-configure the device). Accepted overrides are written to
`/settings.json` *and* applied live — the write happens first, so the device
never claims a change it could not reproduce after a reboot — and are layered on
top of `config.json` at boot (`settingsLoad()` / `settingsOverlay()`), which is
what "clear it" falls back to. `config.json` itself is never rewritten: it is the
file packed into the LittleFS image, and the uploadfs token guard reasons about
it.

**No key, no settings.** With an empty `status_key` the channel answers `403`
rather than accepting unauthenticated writes. A state ping only changes an
animation; these requests persist device configuration, so they do not get a
"dev mode".

## WiFi credentials over the same channel

The device's own setup screen is a 4×32 character grid on a 184×224 panel: a
WPA2 passphrase is 60+ taps and a group switch every few characters. So the
passphrase can be typed on a PC instead and pushed over the channel:

```bash
tools/wifi-setup.sh Home 'hunter2'   # join that network (and keep it)
tools/wifi-setup.sh Home -           # …prompt for the passphrase, not in argv
tools/wifi-setup.sh --forget         # drop them; back to the setup screen
```

| Request | Meaning |
|---|---|
| `?wifi_ssid=NAME&wifi_pass=PASS` | associate with these credentials, and store them **once the link comes up** |
| `?wifi_ssid=&wifi_pass=` | forget: clear NVS and the driver's own AP copy, back to the setup screen |

**It travels over BLE, and that is the point.** A device with no credentials has
no IP address, so the HTTP transport cannot exist yet — `tools/wifi-setup.sh`
tries HTTP first (a re-provision on a device that is already online) and falls
back to `ble-bridge.py --once`, which needs no network at all. The two transports
are the same `statusChannelHandle()` as everything else, so the auth rules are
identical: **no `status_key`, no credentials** (403). Pointing the device at an
attacker's AP is a worse outcome than a wrong brightness, so there is no
"dev mode" here either.

**The passphrase is stored only if the association works.** The request is
recorded, `loop()` starts the association one iteration later, and the credentials
reach NVS when `WL_CONNECTED` fires — the same rule `netConnect()`, the on-device
keyboard path, has always applied. A typo therefore cannot replace a working
network with a useless one, and a failure leaves the old credentials intact (the
panel drops back to the network list, the log says
`wifi: could not connect to "…"; credentials not stored`).

**The reply cannot contain the outcome**, because the association has not happened
yet. It says what was sent, and the answer is the panel
(`Connecting… <ssid>` → `Connected!`, or the network list again) and, after
~15 s, `tools/ble-bridge.py --scan --info` — `ip=0.0.0.0` still means it did
not take.

**Two details worth knowing.**

- *The passphrase is not private on the air.* The HMAC makes the write
  unforgeable, not confidential, and BLE is unauthenticated at the link layer, so
  anyone with a sniffer in radio range during the ~1 s write can read it. The
  alternative — an encrypted characteristic — requires bonding, the ceremony the
  BLE transport deliberately avoids (see below); for credentials typed once on a
  desk that is the wrong trade. Nothing echoes it back either: the reply body,
  the serial log and `status_stub.py` all print the SSID and a redacted
  passphrase (`wifiCredDescribe()`, `wifiQueryForLog()`).
- *Nothing here is written to `/settings.json`.* Credentials go to NVS through
  `hw/net.h` (`netSaveCred()` / `netForgetCred()`, which also clears the driver's
  own copy of the AP config). `/settings.json` is a readable record of overrides,
  and a passphrase does not belong in one; it is also why this is its own request
  kind (`src/parse/wifi_cred_parse.h`, `channelWifi()` in `status_server.cpp`)
  rather than another settings field.

Nothing about the setup screen is removed: the grid is still the path that works
with no PC at all, and picking the SSID the device already has a passphrase for
now prefills it, so reconnecting is one tap on `Connect` instead of a retype.

## The same channel over BLE (when the LAN cannot carry it)

`src/ble_status.cpp` serves the channel above over a small GATT service as well, so
a host that cannot reach the device over IP can still drive it. That is the whole
point: a VLAN boundary, a network that blocks multicast, or an AP that associates
and then carries nothing (docs/network-polling.md) all break an HTTP request — and
none of them mean anything to BLE, which never touches IP.

**Scope: only what the host initiates** — `?state=`, the settings channel and the
WiFi credentials request (which is *only* reachable over BLE until the credentials
work). usage/weather/moon/air are fetches *the device* makes out to the internet
and still need WiFi; BLE cannot stand in for them, and nothing pretends otherwise.
OTA is the one host-initiated path not covered (yet) — `ArduinoOTA` is UDP-based
and has no BLE equivalent here.

**One implementation, two transports.** Both go through `statusChannelHandle()`
(`src/status_channel.h`); a transport only pulls the query and the HMAC pair out of
its wire format. The auth rules, the settings validation and the state machine are
therefore identical, and an untrusted write over BLE is exactly as safe as one over
HTTP — not a second policy that has to be kept in step.

| | HTTP | BLE |
|---|---|---|
| discover | mDNS `usage-monitor.local` | advertise as `usage-monitor`, service `6f4b1e00-…` |
| request | `GET /?<query>` + `X-Ts` / `X-Sig` headers | write `<ts> <sig> <query>` to RX |
| reply | status line + body | notify `<len4><code3><body>` on TX |
| read-back | `?settings` body | read the info characteristic |

**No pairing.** The service asks for no bonding and holds no encrypted
characteristics, so a macOS app connects and writes without the system ever showing
a pairing dialog, and nothing lands in the Bluetooth preferences. The Magic-Trackpad
"plug in the cable once" trick exists because Apple's HID profile needs *bonding*
(an IRK, so it can encrypt and auto-reconnect); none of that applies to a private
GATT service. What bonding would have bought — "only my host may drive this" — is
already the application-layer HMAC, and the frame carries that signature instead of
relying on the link. Requiring encryption here would *require* bonding, i.e. the
ceremony this avoids.

**The host side is `tools/monitor-send.sh`** — one command that owns the ladder, and
what the OpenCode plugin spawns per state change. There is no daemon: it tries the
last URL that worked, then the IP it learned over BLE, then the configured addresses,
then BLE as a one-shot (`ble-bridge.py --once --info`), and exits. The order is a
client-side decision the device cannot make for the host, and the BLE rung only pays
its cold connect (~8-15 s) once HTTP has already failed; on that same link it reads
the device's own IP back, so the next message returns to HTTP even after a DHCP move.

```sh
python3 -m pip install "bleak>=0.22"
tools/ble-bridge.py --scan --info          # is the device in range, and where is it?
tools/ble-bridge.py --once state=done      # drive it without OpenCode (cold connect)
tools/monitor-send.sh done                 # the whole ladder, as the plugin runs it
tools/ble-bridge.py                        # optional: hold the link and serve :8098
```

`tools/ble-bridge.py` with no arguments still serves the channel on
`http://127.0.0.1:8098`, for anything that would rather be an ordinary HTTP client;
the plugin does not need it.

**Finding the device without a serial console.** The Clock page (the device's own
status page) shows the LAN address and whether the peripheral came up — see
[docs/ui-navigation.md](ui-navigation.md#clock-page-device-identity). That matters
here because mDNS does not cross a VLAN boundary, so `usage-monitor.local` may not
resolve at all, and the address is otherwise only in the boot log.

**Reading the state without a reset.** The periodic `diag:` line carries two fields
that exist for this feature:

```
… heap=8115232 psram=8015680 iheap=99300 ble=up online=1 wifi=3 rssi=-52
                              ^^^^^^^^^^^  ^^^^^^
```

`heap=` is dominated by PSRAM and says nothing useful about the memory the BLE stack
competes for; **`iheap=` is the internal figure, and it is the one that matters**
(see the RAM note below). `ble=` is `off` / `failed` / `up` — the state
`ble_status` reached, printed every 10 s, so a bring-up that bailed does not need a
reboot to observe. The info characteristic carries the same numbers, plus the BT
cost measured at bring-up, so `--scan --info` answers the same questions without a
serial console at all.

**RAM is measured, not a constraint.** `bleStatusBegin()` prints the internal heap
around `BLEDevice::init()`, and the figure turns out to be comfortable:

```
ble: internal heap before=228032 after-init=158564 (init took 69468)
ble: internal heap with BT resident=152040
```

BT takes **~68 KB of internal RAM** and leaves ~152 KB before WiFi and the tasks take
their share (~97 KB steady state, dipping to ~50 KB during a TLS handshake). This
mattered enough to measure because 8 MiB of PSRAM buys almost none of it back:
`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096` sends every allocation of 4 KB or less to
internal RAM and Bluedroid is built out of those, while mbedTLS's buffers are
internal too (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC` is off). It is all in the
**prebuilt** platform libraries, so it is not tunable either
(`CONFIG_BT_CTRL_BLE_MAX_ACT`, the two 8 KB BT task stacks, the host buffer counts) —
hence the measurement. `iheap=` on the periodic `diag:` line tracks it at runtime,
and the info characteristic carries it over BLE.

**The advertisement is built by hand, because the library's own path cannot express
it.** `BLEAdvertising::start()` copies `m_advData` into its implicit scan response
*including* `service_uuid_len`, so with a 128-bit service UUID that packet comes out
as UUID(18) + name(15) + txpower(3) = **36 bytes, over the 31-byte limit**. IDF
rejects it and `start()` then `return`s *before* `esp_ble_gap_start_advertising()`
(BLEAdvertising.cpp:266), leaving a live GATT service that never advertises — with
the only explanation on a `log_e` that `CORE_DEBUG_LEVEL=0` compiles out. That was
exactly the observed state (`ble=up`, undiscoverable, silent). So both packets are
now explicit and sized:

```
advertisement  = flags(3) + 128-bit service UUID(18) + manufacturer data(7) = 28
scan response  = the full name                                             = 15
```

and `bleStatusBegin()` refuses to advertise (loudly) if either ever exceeds 31, so
the trap cannot come back quietly.

**Discovery is by manufacturer data, not by service UUID.** macOS never once matched
the 128-bit UUID advertised as AD type 0x07 in an OS-level filtered scan — including
runs where an unfiltered scan matched the same device immediately — and an
unfiltered scan there reported `service_uuids` empty for 40 of 41 nearby devices and
reported no names at all. So the device also advertises a 5-byte manufacturer field
(company `0xFFFF`, magic `E5 1B`, revision `01`) and `tools/ble-bridge.py` matches on
it. The service UUID is still advertised, for anything that can use it.

**A connected peripheral does not advertise** — worth knowing before reading anything
into a scan. While a bridge (or nRF Connect, or any central) holds a link, the device
is out of the air and `--scan` correctly finds nothing; `--once` still works, because
it connects to that same link. This is also why the design is coherent: discovery is
only ever needed when nothing holds a link, which is exactly when the device is
advertising.

**The scan itself is the flaky part, so nothing important depends on it.** Measured
against this device, a 3 s macOS scan matched about 3 times in 5 while the device was
advertising continuously at 200–300 ms (the packets are demonstrably there; the OS
misses them), and an OS-level `service_uuids` filter never matched at all. Hence:
retries (`_ensure` will scan up to three times) and the remembered address, so only a
cold start pays the scan — and the OpenCode plugin's 30 s fallback budget is sized for
that worst case rather than for the ~0.5 s the warm link gives.

Do **not** "fix" this by re-issuing `startAdvertising()` periodically. It looks like
harmless self-healing and it is not: a 5 s re-arm drove `ESP_GAP_BLE_ADV_START_COMPLETE_EVT`
nine times in 45 s and made the device *less* visible (0 matches in 5 scans, against
3 in 5 without it) — restarting the advertiser is not free. The one re-issue that
belongs is the one in `onDisconnect()`, the library's own idiom.

**Radio cost.** The BLE stack is a second user of the radio the pollers fight over:
the peripheral advertises every 200–300 ms (it started at ~1 s, which turned out to
be too slow to be *reliably* discovered — a 6 s macOS scan heard too few packets and
would intermittently match nothing), and a connected peer is asked for a slow
connection interval (70–100 ms, latency 4). BLE is only ever used when the LAN path
is *already* broken. **The coexistence numbers have not been measured** — re-run
`tools/link_probe.py` for the `http: … tls=` figures before trusting them, exactly as
with any other radio change (docs/power.md). Flash grew by ~590 KB: the Bluedroid
host and controller are compiled into the platform's libraries but were only *linked
in* once something referenced them (`ble_status` on `/config.json` turns the
peripheral back off at runtime; the code stays in the image either way), and the
`-DBLE_DEBUG_LOG` build adds another ~90 KB.

**Connecting is the slow part, so a fallback pays it once per message.** Measured
against this device over macOS:

| | |
|---|---|
| warm request (link up) | **~0.5 s** |
| cold connect, first attempt | 8–15 s — often times out, then the retry succeeds |

The one-shot (`--once`), which is what the ladder uses, always pays the cold connect;
the plugin's 60 s budget is sized for that, not for the ~0.5 s the LAN rungs take.
What softens it is the remembered address in
`~/.cache/esp32-usage-monitor/ble-device.json`: a later run skips the scan (the flaky
part on macOS) and pays only the connect. A bridge process (`tools/ble-bridge.py`
with no arguments) can still hold the link for ~0.5 s requests — default `--idle 70`,
deliberately just under the device's own 90 s backstop — for a host that would rather
run a daemon than pay a cold connect per message; the plugin does not need it. Both
are optimizations with fallbacks: a stale address or a dropped link just means paying
the cold path once.

**Bluedroid's own error output needs a flag to see.** The messages that explain an
advertising failure (`esp_ble_gap_config_adv_data: rc=…`,
`esp_ble_gap_start_advertising: rc=…`) come from the *Arduino* BLE library, which
PlatformIO compiles with this project's flags — at `CORE_DEBUG_LEVEL=0` every `log_*`
in it is compiled out. (IDF's prebuilt libraries, `libbt.a` included, keep theirs:
`CONFIG_LOG_DEFAULT_LEVEL=1`. A controller-level failure is visible; an
advertising-level one is not.) To see them:

```sh
PLATFORMIO_BUILD_FLAGS='-DBLE_DEBUG_LOG -DCORE_DEBUG_LEVEL=4' \
  pio run -e esp32-usage-monitor -t clean -t upload
```

`-t clean` is not optional when switching between the two builds: `PLATFORMIO_BUILD_FLAGS`
is a runtime override that PlatformIO does not hash, so a plain follow-up build will
keep the previous build's library objects and produce a hybrid binary (this cost an
afternoon of contradictory `strings` results). `BLE_DEBUG_LOG` also raises the runtime
log level, because the default (ERROR) would filter out the `log_d` lines describing
what `start()` was asked to advertise.

**Capturing a boot log.** Opening `/dev/cu.usbmodem*` does **not** reset this board,
and `pio run -t upload` cannot be used while a capture holds the port. Use the reset
flag instead:

```sh
tools/serial_capture.py /dev/cu.usbmodem1101 25 --reset-run
```

(`--jtag-reset` is the other half: it reboots into the ROM download mode, i.e. it
parks the chip waiting to be flashed.)

