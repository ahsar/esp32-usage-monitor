# esp32-usage-monitor — OpenCode plugin

Pushes OpenCode's agent activity to the ESP32 usage monitor so its panel shows
a **Thinking…** / **Needs you** animation and plays a completion beep.

The device serves the same tiny channel two ways — HTTP on port 80
(`src/status_server.cpp`) and a BLE GATT service (`src/ble_status.cpp`) — and this
plugin is the client for both. It is the *missing half*: without it nothing ever
drives the device's status endpoint.

It sends over an **ordered list of transports**, because the two do not always agree
about being reachable. A LAN that crosses a VLAN boundary, blocks mDNS, or rides an
AP that associates and then carries nothing breaks HTTP without the device noticing
— and none of that means anything to BLE. The order is a client-side decision: the
device cannot choose how the host reaches it.

## What it sends

| OpenCode V2 event | Device request | Effect |
| --- | --- | --- |
| `session.execution.started` | `GET /?state=running` | "Thinking…" animation, wakes the panel |
| `permission.asked` | `GET /?state=waiting` | "Needs you" animation + attention beep |
| `session.execution.succeeded` | `GET /?state=done` | rising two-beep cue |
| `session.execution.interrupted` | `GET /?state=done` | rising two-beep cue |
| `session.execution.failed` | `GET /?state=failed` | low beep |
| `permission.replied` / `permission.rejected` | `GET /?state=running` | back to working |

Only *transitions* are sent (duplicate states are dropped), and each request is
fire-and-forget with a short timeout — a device that is off, asleep, or on another
network can never slow OpenCode down.

> The event names are OpenCode **V2** names, verified against `opencode
> v2.0.x`. V1's `session.idle` / `session.status` / `permission.updated` are
> not emitted by V2.

## Transports

`options.urls` is tried in order, and a transport that fails is put in a penalty box
(`penaltyMs`, 30 s) so a dead one is not paid for on every message. Expiry is also
the recovery path: once it lapses the LAN is preferred again, so a network that came
back is picked up on the next message without any restart.

| # | Transport | Default | Timeout |
| --- | --- | --- | --- |
| 1 | the LAN, over HTTP | `http://usage-monitor.local` | `timeoutMs` (1.5 s) |
| 2… | any further addresses | — | `fallbackTimeoutMs` (30 s) |
| last | the BLE bridge | `http://127.0.0.1:8098` | `fallbackTimeoutMs` (30 s) |

More than one LAN address is the norm on a segmented network: mDNS does not cross a
VLAN boundary, so list the device's IP on each segment you might be on and let the
order sort it out.

The last rung is `tools/ble-bridge.py`, which holds a BLE link to the device and
serves the same channel on loopback. Its budget is deliberately enormous next to the
LAN's 1.5 s, because **connecting is the slow part**: measured against the device,
a request on a link the bridge already holds takes ~0.5 s, while a cold connect takes
8-15 s (macOS often times out the first attempt and succeeds on the retry). A cold
fallback abandoned after 1.5 s could never work at all. `"ble": false` takes the
rung out; leave it running (the bridge keeps the link for 70 s of quiet — just
under the device's own backstop, because a *stale* link makes the device
undiscoverable — and remembers the device's address, so only a cold start pays the
scan).

Messages are delivered **one at a time, in order** — not for the device's sake (it
takes one at a time anyway) but so that a 1.5 s LAN probe cannot overlap the next
event, which would make both pay the same dead transport's timeout and neither
record the penalty. Serialising costs nothing here: a message can be late, but it is
never lost, and the chimes depend on seeing the states in the order they happened.

A `401` stops the walk rather than falling through: the key is wrong, and it will be
just as wrong on the next transport. Nothing answering at all is not an error —
the message is dropped and the next event retries.

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
"<state>|<ts>")`. The device verifies the signature in constant time and, once
its clock is NTP-synced, rejects anything outside a ±60 s window — so requests
cannot be forged or replayed. Requests without a valid signature get `401` and
change nothing.

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

### 2. Start the BLE bridge (optional, for when the LAN is not there)

Skip this if HTTP always reaches the device. Otherwise:

```sh
python3 -m pip install "bleak>=0.22"
../ble-bridge.py --scan --info   # is the device advertising? what does it say about itself?
../ble-bridge.py                 # serve http://127.0.0.1:8098
```

`--scan --info` is also the only read-out that needs no serial console: it prints the
device's own report (`fw=… ip=… auth=… ble=… iheap=… bt=…`). `--once state=done`
sends one request by hand, but pays a full cold connect (~8-15 s) every time — the
server mode is the warm path.

It needs no pairing and no key: it forwards the plugin's signed request verbatim. Run
it wherever it can hear the device (i.e. next to it, not on a server), and keep it
running — a bridge that is not up is simply treated as another dead transport.

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

### 4. Restart the OpenCode service

Plugins are loaded by the background service, so restart it after changing the
config or the plugin:

```sh
opencode service restart
```

## Debugging

Set `ESP32_MONITOR_DEBUG=1` **before the service starts** (the service, not the
TUI, reads it) to log every state transition, which transport carried it, and every
failure. Without it the plugin is completely silent. The transport line printed at
startup shows the ladder and the timeout each rung got:

```
[esp32-monitor] transports: usage-monitor.local(1500ms) → 127.0.0.1:8098(8000ms)
```

The quickest end-to-end check needs no OpenCode at all:

```sh
../status-ping.sh running    # device should show "Thinking…"
../status-ping.sh waiting    # …then "Needs you"
../status-ping.sh done       # …then a rising chime
```

To check the *fallback* path on its own, go straight at the bridge:

```sh
../ble-bridge.py --once state=done     # expect: 200 OK
../ble-bridge.py --scan --info         # firmware, IP and whether auth is on
```

To exercise the plugin against a stand-in instead of the hardware (including the
HMAC path), run `python3 ../status_stub.py <status_key>` and put
`http://127.0.0.1:8099` first in `urls`.

