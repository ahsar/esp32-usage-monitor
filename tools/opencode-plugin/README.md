# esp32-usage-monitor — OpenCode plugin

Pushes OpenCode's agent activity to the ESP32 usage monitor so its panel shows
a **Thinking…** / **Needs you** animation and plays a completion beep.

The device serves the same tiny channel two ways — HTTP on port 80
(`src/status_server.cpp`) and a BLE GATT service (`src/ble_status.cpp`) — and this
plugin is the client for both. It is the *missing half*: without it nothing ever
drives the device's status endpoint.

The plugin itself is deliberately thin: it maps an event to a state and runs
`tools/monitor-send.sh`, which owns the transport ladder. That is a command, not a
daemon — every message tries the transports in order, uses the first that answers,
and exits:

1. the URL that worked last (cached),
2. the device IP learned over BLE (cached),
3. `options.urls` (default `http://usage-monitor.local`),
4. BLE, one shot (`tools/ble-bridge.py --once --info`).

The order is a client-side decision — the device cannot choose how the host reaches
it — and it matters: a LAN that crosses a VLAN boundary, blocks mDNS, or rides an AP
that associates and then carries nothing breaks HTTP without the device noticing, and
none of that means anything to BLE. A message that falls all the way through pays one
cold BLE connect (~8-15 s on macOS); every other path is sub-second. A successful BLE
send reads the device's own IP back and caches it, so the next message returns to HTTP
even after a DHCP move.

## What it sends

| OpenCode V2 event | Device request | Effect |
| --- | --- | --- |
| `session.execution.started` | `GET /?agent=1&state=running&session=…&repo=…&tool=…` | "Thinking…" + repo/tool/elapsed, wakes the panel |
| `permission.asked` / `question.asked` | `GET /?agent=1&state=waiting&session=…&repo=…&note=…` | "Needs you" + *what* it is waiting for + attention beep |
| `session.execution.succeeded` | `GET /?agent=1&state=done&session=…` | rising two-beep cue |
| `session.execution.interrupted` | `GET /?agent=1&state=done&session=…` | rising two-beep cue |
| `session.execution.failed` | `GET /?agent=1&state=failed&session=…` | low beep |
| `permission.replied` / `permission.rejected` | `GET /?agent=1&state=running&session=…&…` | back to working |
| `question.replied` / `question.rejected` | `GET /?agent=1&state=running&session=…&…` | back to working |

Only *transitions* are sent — the key is the whole query, so a repeat of the same
state *and* context is dropped while a different tool inside one running session
still repaints — and each request is fire-and-forget with a short timeout. A
device that is off, asleep, or on another network can never slow OpenCode down.

**`session` is what makes two windows work.** The plugin maps each OpenCode
session id to a short token (`s1`, `s2`, …) and sends it on every state,
`done`/`failed` included, so the device removes the right entry instead of
clearing "the" agent state. Two windows working at once show `2 running`; a
`waiting` in either one owns the screen. While a session is active the plugin also
**re-sends its last state every ten minutes** (`heartbeatMs`): the device treats a
repeat as a no-op on screen, but it keeps the session alive, so a host that
crashes is forgotten after 45 minutes instead of leaving "Thinking…" up for hours.
A heartbeat cannot re-open a screen you dismissed with Key1; only a state change
does.

**Context (the `repo`/`tool`/`note` fields).** `repo` is the basename of the
OpenCode server's working directory — the session's project root. `tool` is the
permission that was asked for and `note` its first pattern (or the question's
header), collapsed to one line and clipped to the device's cap. They are read
defensively from the event payload: a server that renames a field degrades to
"no tool"/"no note", never to a crash. This is *why* the request is the `agent=`
form and not `?state=`: the device signs the whole raw query for it, so a LAN peer
cannot edit the text a captured ping would put on the panel (`docs/pc-bridge.md`).
`done`/`failed` deliberately send no context, so the device's copy is cleared and
the next run cannot show a stale repo.

> The event names are OpenCode **V2** names, verified against `opencode
> v2.0.x`. V1's `session.idle` / `session.status` / `permission.updated` are
> not emitted by V2.

## The ladder

Each message runs `tools/monitor-send.sh <query>` (the full
`agent=1&state=…&repo=…` string), which tries, in order:

| # | Rung | Budget |
| --- | --- | --- |
| 1 | the URL that worked last (cached) | `ESP32_MONITOR_HTTP_TIMEOUT_S` (2 s) |
| 2 | the device IP learned over BLE (cached) | same |
| 3 | `options.urls` (default `http://usage-monitor.local`) | same |
| 4 | BLE, one shot (`ble-bridge.py --once --info`) | one cold connect, ~8-15 s |

A message is fire-and-forget: the plugin allows it `sendTimeoutMs` (60 s — the whole
ladder, worst case) and then kills it, and a message that reached nothing puts the
device in a penalty box (`penaltyMs`, 30 s) so a transport that is down is not paid
for on every event. Expiry is also the recovery path: the next message walks the
ladder afresh, so a network that came back is picked up without a restart.

Any HTTP *response* — `401`/`403` included — counts as reached: the key is wrong, and
it will be just as wrong on the next rung, so the ladder stops there rather than
spending the BLE budget. Nothing answering at all is not an error; the message is
dropped and the next event retries.

More than one LAN address is the norm on a segmented network: mDNS does not cross a
VLAN boundary, so list the device's IP — on each segment you might be on — in `urls`
and let the order sort it out. The address is on the device's Clock page and is read
back by `tools/ble-bridge.py --scan --info` (`ip=…`), which is also how the cache
learns it: after a BLE fallback the next message prefers that IP over mDNS.

Messages are delivered **one at a time, in order**, so the chimes see the states in
the order they happened — and so a slow BLE fallback cannot be overlapped by the next
event's probe. `"ble": false` takes the BLE rung out.

## Security

The endpoint is reachable by anyone on the LAN, so the device signs every state
change. Set a shared secret in the device's `data/config.json`:

```sh
python3 - <<'PY'
import json, secrets, pathlib
p = pathlib.Path("data/config.json")
c = json.loads(p.read_text())
c["status_key"] = secrets.token_hex(32)
p.write_text(json.dumps(c, indent=2) + "\n")
print("status_key set — re-run: pio run -e esp32-usage-monitor -t uploadfs")
PY
```

Then pass the same value to the plugin:

```jsonc
{
  "plugins": [
    {
      "package": "/Users/lishuo/dev/esp32-usage-monitor/tools/opencode-plugin",
      "options": {
        "urls": [
          "http://usage-monitor.local",
          "http://192.168.20.31"
        ],
        "key": "<the same status_key>"
      }
    }
  ]
}
```

The plugin sends `X-Ts` (unix seconds) and `X-Sig` = `HMAC-SHA256(key,
"<payload>|<ts>")`, where the payload is the whole raw query for the `agent=`
form the plugin now sends (and just the state word for a bare `?state=`). The
device verifies the signature in constant time and, once its clock is NTP-synced,
rejects anything outside a ±60 s window — so requests cannot be forged or
replayed, and the context text on the animation screen cannot be edited by a LAN
peer who captured a ping. Requests without a valid signature get `401` and change
nothing; with no `status_key` the check is skipped entirely (bench mode), exactly
as it is for `?state=`.

> The key itself never goes on the wire (only the signature and timestamp do),
> so plain HTTP does not leak the secret. What it does allow is a captured
> request to be replayed inside the 60 s window — TLS is a separate, heavier
> option (an mbedTLS server on the ESP32) if that matters.

With no `status_key` the device logs `status auth: DISABLED` at boot and accepts
anything — fine for a bench, not for an office network.

## Setup

### 1. Address the device

The firmware advertises itself over mDNS as `usage-monitor.local`. Confirm it
resolves from this machine:

```sh
ping usage-monitor.local          # or: curl http://usage-monitor.local/?state=done
```

If your network blocks multicast DNS — common as soon as there is more than one
VLAN — use the device's IP instead (it is printed on the serial console at boot next
to `diag: ip=`, and readable over BLE from the info characteristic).

### 2. Nothing to start for the fallback (but install `bleak`)

There is no bridge daemon to run: the BLE rung is a one-shot
(`tools/ble-bridge.py --once`) spawned only when the LAN rungs fail, so the only
setup is its one dependency:

```sh
python3 -m pip install "bleak>=0.22"
../ble-bridge.py --scan --info   # is the device advertising? what does it say about itself?
```

`--scan --info` is also the only read-out that needs no serial console: it prints the
device's own report (`fw=… ip=… auth=… ble=… iheap=… bt=…`), which is where an IP for
`urls` comes from. Neither needs pairing or a key: the one-shot signs for itself, from
`data/config.json` (or `ESP32_MONITOR_KEY`). Run it wherever it can hear the device —
next to it, not on a server.

### 3. Load the plugin

Add it to the **global** config so every project reports to the device
(`~/.config/opencode/opencode.json`):

```jsonc
{
  "plugins": [
    {
      "package": "/Users/lishuo/dev/esp32-usage-monitor/tools/opencode-plugin",
      "options": { "urls": ["http://usage-monitor.local"] }
    }
  ]
}
```

`package` must be an absolute path (or a published package name). `urls` may be
omitted, in which case `url` is used if set, then the `ESP32_MONITOR_URLS`
(comma-separated) / `ESP32_MONITOR_URL` environment variables, and finally
`http://usage-monitor.local`. The bridge is appended unless `"ble": false`.
`heartbeatMs` (default 600000) is how often an active session re-sends its state
so a crashed host is forgotten; set it to 0 to disable the heartbeat (the device
then drops the screen 45 minutes after the last real event).

### 4. Restart the OpenCode service

Plugins are loaded by the background service, so restart it after changing the
config or the plugin:

```sh
opencode service restart
```

## Debugging

Set `ESP32_MONITOR_DEBUG=1` **before the service starts** (the service, not the
TUI, reads it) to log every state transition and every failure. Without it the plugin
is silent apart from real errors. The line printed at startup shows the ladder it
will run:

```
[esp32-monitor] ladder: http://usage-monitor.local → http://172.16.29.91 via …/monitor-send.sh (auth off)
```

The quickest end-to-end check is the very command the plugin runs, and it prints
which rung carried the message:

```sh
ESP32_MONITOR_DEBUG=1 ../monitor-send.sh running    # device should show "Thinking…"
ESP32_MONITOR_DEBUG=1 ../monitor-send.sh waiting    # …then "Needs you" + a beep
ESP32_MONITOR_DEBUG=1 ../monitor-send.sh done       # …then a rising chime

# the context form, exactly as the plugin sends it (signed over the whole query)
ESP32_MONITOR_DEBUG=1 ../monitor-send.sh \
  'agent=1&state=running&repo=esp32-usage-monitor&tool=bash'
ESP32_MONITOR_DEBUG=1 ../monitor-send.sh \
  'agent=1&state=waiting&repo=esp32-usage-monitor&note=Allow%20bash%20to%20run'
```

To check the *fallback* rung on its own, hit BLE directly (a cold connect, ~8-15 s):

```sh
../ble-bridge.py --once state=done --info   # expect: 200 OK, then the device's info
../ble-bridge.py --scan --info              # firmware, IP and whether auth is on
```

To exercise the ladder against a stand-in instead of the hardware (including the HMAC
path), run `python3 ../status_stub.py <status_key>` and point the first rung at it:

```sh
python3 ../status_stub.py <status_key> &
ESP32_MONITOR_URLS=http://127.0.0.1:8099 ESP32_MONITOR_BLE=0 ../monitor-send.sh done
```

