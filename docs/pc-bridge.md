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
