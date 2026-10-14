# esp32-usage-monitor — OpenCode plugin

Pushes OpenCode's agent activity to the ESP32 usage monitor so its panel shows
a **Thinking…** / **Needs you** animation and plays a completion chime.

The device runs a tiny HTTP server on port 80 (`src/status_server.cpp`); this
plugin is the client. It is the *missing half* — without it nothing ever drives
the device's status endpoint.

## What it sends

| OpenCode V2 event | Device request | Effect |
| --- | --- | --- |
| `session.execution.started` | `GET /?state=running` | "Thinking…" animation, wakes the panel |
| `permission.asked` | `GET /?state=waiting` | "Needs you" animation + attention chime |
| `session.execution.succeeded` | `GET /?state=done` | rising chime |
| `session.execution.interrupted` | `GET /?state=done` | rising chime |
| `session.execution.failed` | `GET /?state=failed` | falling chime |
| `permission.replied` / `permission.rejected` | `GET /?state=running` | back to working |

Only *transitions* are sent (duplicate states are dropped), and each request is
fire-and-forget with a 1.5 s timeout — a device that is off, asleep, or on
another network can never slow OpenCode down.

> The event names are OpenCode **V2** names, verified against `opencode
> v2.0.x`. V1's `session.idle` / `session.status` / `permission.updated` are
> not emitted by V2.

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
        "url": "http://usage-monitor.local",
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

If your network blocks multicast DNS, use the device's IP instead (it is
printed on the serial console at boot next to `diag: ip=`).

### 2. Load the plugin

Add it to the **global** config so every project reports to the device
(`~/.config/opencode/opencode.json`):

```jsonc
{
  "plugins": [
    {
      "package": "/Users/lishuo/dev/esp32-usage-monitor/tools/opencode-plugin",
      "options": { "url": "http://usage-monitor.local" }
    }
  ]
}
```

`package` must be an absolute path (or a published package name). `url` may be
omitted to fall back to the `ESP32_MONITOR_URL` environment variable and then
`http://usage-monitor.local`.

### 3. Restart the OpenCode service

Plugins are loaded by the background service, so restart it after changing the
config or the plugin:

```sh
opencode service restart
```

## Debugging

Set `ESP32_MONITOR_DEBUG=1` **before the service starts** (the service, not the
TUI, reads it) to log every state transition and failure. Without it the plugin
is completely silent.

The quickest end-to-end check needs no OpenCode at all:

```sh
../status-ping.sh running    # device should show "Thinking…"
../status-ping.sh waiting    # …then "Needs you"
../status-ping.sh done       # …then a rising chime
```

To exercise the plugin against a stand-in instead of the hardware (including the
HMAC path), run `python3 ../status_stub.py <status_key>` and point the plugin's
`url` at `http://127.0.0.1:8099`.
