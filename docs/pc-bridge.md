# PC bridge and chimes

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

**Chimes.** Synthesised at boot (`src/voice/chime.*`) into a 500 ms PCM buffer
and streamed from `loop()`; there is no raw file, so activating a chime never
requires an `uploadfs` (which would erase the device's rotated tokens).

## Settings over the same endpoint

The same port carries a second, small protocol: the settings channel. It exists
because the alternative for every knob below is editing `data/config.json` and
running `uploadfs`, which **erases the device's rotated OAuth token** (see
`tools/uploadfs_guard.py`) — far too much ceremony for "make the chimes stop".

| Request | Meaning |
|---|---|
| `?chime=0` / `?chime=1` | chime on completion / needs-you |
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
