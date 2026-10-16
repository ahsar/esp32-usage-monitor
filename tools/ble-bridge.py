#!/usr/bin/env python3
"""BLE bridge for the ESP32 usage monitor — the LAN's fallback transport.

The device serves its status/settings channel over HTTP *and* over a small BLE
GATT service (`src/ble_status.cpp`). This tool is the PC side of the second one:
it holds a BLE link to the device and exposes the *same* channel as plain HTTP on
loopback, so a client can use it without knowing anything about BLE.

    device channel  =  http://usage-monitor.local/         (LAN, preferred)
                    =  http://127.0.0.1:8098/              (this bridge, BLE)

Why a separate process rather than BLE inside the OpenCode plugin: the plugin
runs on Bun, and every Node BLE binding on macOS is a native addon with a shaky
build story. `bleak` talks to CoreBluetooth directly, needs no compilation, and
can be driven by hand — so the fragile part can be tested without OpenCode.

**No pairing.** The device's service is unencrypted on purpose, and CoreBluetooth
lets an app connect to an unencrypted peripheral without any OS-level pairing, so
nothing pops a dialog and nothing is written to the Bluetooth preferences. The
device does not trust the link either: every request carries the same HMAC-SHA256
signature the HTTP path verifies, so an unpaired, unencrypted link changes
nothing about who may drive the device.

Requests are forwarded verbatim — including `X-Ts` / `X-Sig` — so this bridge
never needs the shared key. The one exception is `--once`, which signs for you so
the path can be exercised by hand.

    python3 -m pip install "bleak>=0.22"      # once
    tools/ble-bridge.py --scan                # is the device in range?
    tools/ble-bridge.py --once state=done     # drive it by hand
    tools/ble-bridge.py                       # serve on 127.0.0.1:8098
"""

from __future__ import annotations

import argparse
import asyncio
import hashlib
import hmac
import json
import os
import pathlib
import signal
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    from bleak import BleakClient, BleakScanner
    from bleak.exc import BleakError
except ImportError:  # pragma: no cover - the message is the point
    sys.stderr.write(
        "ble-bridge: the `bleak` package is required.\n"
        '  python3 -m pip install "bleak>=0.22"\n'
    )
    raise SystemExit(2)

# Must match src/ble_status.cpp.
SERVICE_UUID = "6f4b1e00-9a3c-4d5e-8f21-7c0d9e5a4b30"
RX_UUID = "6f4b1e01-9a3c-4d5e-8f21-7c0d9e5a4b30"
TX_UUID = "6f4b1e02-9a3c-4d5e-8f21-7c0d9e5a4b30"
INFO_UUID = "6f4b1e03-9a3c-4d5e-8f21-7c0d9e5a4b30"
MFR_COMPANY = 0xFFFF
MFR_MAGIC = b"\xe5\x1b"

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 8098
# Discovery is retried, so this is per *attempt*: keep it short enough that the
# whole scan → connect → write sequence stays inside the client's fallback budget
# (the OpenCode plugin allows 8 s for this transport by default).
DEFAULT_SCAN_S = 3.0
DEFAULT_CONNECT_TRIES = 2
DEFAULT_QUERY_TIMEOUT_S = 8.0
# Long enough for macOS to finish a connect: measured 8-15 s cold (the first
# attempt often times out and the retry succeeds), against ~0.5 s once the link is
# up. See --idle for why staying up is the point.
DEFAULT_CONNECT_TIMEOUT_S = 12.0
# Dropping the link after a minute is a false economy when a cold reconnect costs
# 8-15 s — but holding one forever is worse than that: a peripheral stops
# advertising while connected, so a *stale* link makes the device undiscoverable.
# 70 s therefore sits just under the device's own 90 s backstop, so the bridge lets
# go deliberately (and the device sees a clean disconnect, after which it advertises
# again) instead of the device timing it out behind our back.
DEFAULT_IDLE_S = 70.0
# Where the device's OS-level address is remembered between runs (see
# Link._remember). Under ~/.cache so a checkout stays clean.
DEFAULT_STATE = pathlib.Path.home() / ".cache" / "esp32-usage-monitor" / "ble-device.json"
CONFIG = pathlib.Path(__file__).resolve().parent.parent / "data" / "config.json"


def log(msg: str) -> None:
    print(f"[ble-bridge] {msg}", file=sys.stderr, flush=True)


# ---------------------------------------------------------------------------
# the link
# ---------------------------------------------------------------------------


def looks_like_device(device, adv) -> bool:
    """Judge an advertisement by whatever the OS actually reported.

    On macOS an unfiltered scan reported `service_uuids` empty for 40 of 41 nearby
    devices and reported no names at all, so neither can be relied on alone. The
    device therefore carries a manufacturer-specific marker, which CoreBluetooth
    surfaces as a first-class field.
    """
    if SERVICE_UUID.lower() in {u.lower() for u in (adv.service_uuids or [])}:
        return True
    if (adv.manufacturer_data or {}).get(MFR_COMPANY, b"")[:2] == MFR_MAGIC:
        return True
    return (device.name or adv.local_name or "").lower() == "usage-monitor"


class LinkError(RuntimeError):
    """The device could not be reached over BLE (as opposed to answering no)."""


class Link:
    """A BLE link to the device, kept open across requests.

    One request is in flight at a time — that is the device's own contract (it
    has a single frame buffer and the client waits for the reply notification
    before writing again), and it is also what makes the reply unambiguous.
    """

    def __init__(self, scan_s: float, timeout_s: float, idle_s: float,
                 tries: int = DEFAULT_CONNECT_TRIES,
                 connect_timeout_s: float = DEFAULT_CONNECT_TIMEOUT_S,
                 state_path: pathlib.Path | None = None) -> None:
        self.scan_s = scan_s
        self.timeout_s = timeout_s
        self.connect_timeout_s = connect_timeout_s
        self.idle_s = idle_s
        self.tries = max(1, tries)
        self.state_path = state_path or DEFAULT_STATE
        self._preset = None
        # The device's address as this host knows it, from a previous run: see
        # _remember(). Connecting straight to it skips the scan entirely.
        self._cached = self._load_cached()
        self._client: BleakClient | None = None
        self._device = None
        self._lock = asyncio.Lock()
        self._rx_buf = bytearray()
        self._rx_event = asyncio.Event()
        self._last_used = 0.0
        self._idle_task: asyncio.Task | None = None

    # -- discovery ---------------------------------------------------------

    async def _scan_once(self):
        """One unfiltered scan pass; returns a matching device, or None."""
        hits: dict[str, tuple] = {}

        def cb(device, adv):
            if looks_like_device(device, adv):
                hits[device.address] = (device, adv)

        scanner = BleakScanner(detection_callback=cb)
        await scanner.start()
        try:
            deadline = time.monotonic() + self.scan_s
            while time.monotonic() < deadline:
                if hits:
                    break
                await asyncio.sleep(0.2)
        finally:
            await scanner.stop()
        return next(iter(hits.values()))[0] if hits else None

    async def _find(self):
        """Scan until something matches, or give up.

        One unfiltered pass, matching on the manufacturer marker. A
        `service_uuids`-filtered pass is deliberately *not* used: macOS never once
        matched our 128-bit UUID advertised as AD type 0x07 (the OS-level filter
        came back empty on every attempt, including runs where the unfiltered pass
        matched immediately), so it would only spend a whole scan window to fail.
        """
        try:
            return await self._scan_once()
        except Exception as exc:  # noqa: BLE001 - bleak versions raise broadly
            raise LinkError(f"scan failed: {exc}") from exc

    async def _ensure(self):
        if self._client is not None and self._client.is_connected:
            return
        self._client = None

        # Candidates in the order they are worth trying. A remembered address
        # needs no scan at all, which is what makes repeated requests fast and
        # reliable — the scan is the flaky part on macOS (a fresh CBCentralManager
        # per invocation sometimes misses a device that is demonstrably
        # advertising), and paying it once per message is the difference between
        # "usually works" and "works".
        targets: list = []
        if self._preset is not None:
            targets.append(self._preset)
        if self._cached:
            targets.append(self._cached)
        # Bounded to fit the client's budget: a cached connect (12 s worst case),
        # then one scan (3 s) and its connect. The retry that matters is the
        # *next message* — the client's penalty box expires and it comes back —
        # not a longer stall here, which would only delay everything queued behind
        # it. A direct connect is tried before scanning on purpose: it works even
        # when the device is not advertising (because something still holds a
        # link), which is exactly when scanning cannot.
        targets += [None]

        last = ""
        for target in targets:
            device = target
            if device is None:
                log(f"scanning for {SERVICE_UUID} ({self.scan_s:g}s)…")
                device = await self._find()
            if device is None:
                last = "not found"
                continue

            name = getattr(device, "name", None) or device
            log(f"connecting to {name}…")
            client = BleakClient(device, timeout=self.connect_timeout_s)
            try:
                await client.connect()
                await client.start_notify(TX_UUID, self._on_notify)
            except Exception as exc:  # noqa: BLE001
                last = f"{type(exc).__name__}: {exc}"
                log(f"connect failed ({last})")
                try:
                    await client.disconnect()
                except Exception:  # noqa: BLE001
                    pass
                continue

            if not isinstance(device, str):
                self._remember(device)
            self._preset = None
            self._client = client
            log(f"connected (mtu={getattr(client, 'mtu_size', None) or '?'})")
            self._idle_task = asyncio.create_task(self._idle_watch())
            return
        raise LinkError(f"could not connect over BLE ({last})")

    def preset(self, device) -> None:
        """Try this device first instead of scanning for it (see _ensure)."""
        self._preset = device

    # -- remembered address -------------------------------------------------

    def _load_cached(self):
        try:
            return json.loads(self.state_path.read_text()).get("address")
        except Exception:  # noqa: BLE001 - absent or corrupt: just scan
            return None

    def _remember(self, device) -> None:
        """Persist the OS's handle for the device so the next run skips the scan.

        On macOS `device.address` is a CoreBluetooth UUID, stable for this host,
        and bleak accepts it for a direct connect. Only an optimization: a failure
        to use it just falls back to scanning, and a fresh machine pays one scan.
        """
        self._cached = device.address
        try:
            self.state_path.parent.mkdir(parents=True, exist_ok=True)
            self.state_path.write_text(json.dumps({"address": device.address}) + "\n")
        except Exception as exc:  # noqa: BLE001
            log(f"could not cache the device address ({exc}); will keep scanning")

    # -- connection --------------------------------------------------------

    async def _idle_watch(self):
        """Drop the link when nothing is using it.

        A BLE link that is left connected holds radio slots on the device that
        its WiFi TLS handshakes want (docs/power.md), and the device is on WiFi
        whenever the LAN path works — which is most of the time.
        """
        try:
            while True:
                await asyncio.sleep(1.0)
                if self._client is None:
                    return
                if time.monotonic() - self._last_used >= self.idle_s:
                    log("idle, disconnecting")
                    await self.close()
                    return
        except asyncio.CancelledError:
            raise

    async def close(self):
        if self._idle_task is not None:
            self._idle_task.cancel()
            self._idle_task = None
        if self._client is not None:
            try:
                await self._client.disconnect()
            except Exception:  # noqa: BLE001 - already gone is fine
                pass
            self._client = None

    # -- requests ----------------------------------------------------------

    def _on_notify(self, _char, data: bytearray) -> None:
        self._rx_buf.extend(data)
        if len(self._rx_buf) >= 4:
            try:
                want = int(bytes(self._rx_buf[:4]), 16)
            except ValueError:
                want = -1
            if want >= 0 and len(self._rx_buf) >= want:
                self._rx_event.set()

    async def request(self, query: str, ts: str, sig: str) -> tuple[int, str]:
        """Send one request; return (status code, body). Raises LinkError."""
        async with self._lock:
            await self._ensure()
            assert self._client is not None

            frame = f"{ts} {sig} {query}".encode()
            self._rx_buf = bytearray()
            self._rx_event = asyncio.Event()

            try:
                await self._client.write_gatt_char(RX_UUID, frame, response=True)
                await asyncio.wait_for(self._rx_event.wait(), self.timeout_s)
            except LinkError:
                raise
            except Exception as exc:  # noqa: BLE001
                await self.close()
                raise LinkError(f"write/reply failed: {exc}") from exc
            finally:
                self._last_used = time.monotonic()

            buf = bytes(self._rx_buf)
            if len(buf) < 7:
                raise LinkError(f"short reply ({len(buf)} bytes)")
            total = int(buf[:4], 16)
            if len(buf) != total:
                raise LinkError(f"truncated reply ({len(buf)}/{total} bytes)")
            code, body = buf[4:7].decode("ascii", "replace"), buf[7:].decode("utf-8", "replace")
            try:
                return int(code), body
            except ValueError:
                raise LinkError(f"unparsable reply header {code!r}") from None

    async def info(self) -> str:
        async with self._lock:
            await self._ensure()
            assert self._client is not None
            try:
                raw = await self._client.read_gatt_char(INFO_UUID)
                return bytes(raw).decode("utf-8", "replace")
            except Exception as exc:  # noqa: BLE001
                raise LinkError(f"read failed: {exc}") from exc


# ---------------------------------------------------------------------------
# signing (only for --once, so the device path can be driven by hand)
# ---------------------------------------------------------------------------


def load_key(explicit: str | None) -> str:
    if explicit is not None:
        return explicit
    env = os.environ.get("ESP32_MONITOR_KEY")
    if env:
        return env
    try:
        return json.loads(CONFIG.read_text()).get("status_key", "") or ""
    except Exception:  # noqa: BLE001
        return ""


def signed_payload(query: str) -> str:
    """What the device signs: the state *value*, or the whole query for settings.

    Mirrors status_server.cpp — a `state=` request keeps the original value-only
    signature so the OpenCode plugin never had to change.
    """
    if query.startswith("state="):
        return query[len("state="):]
    return query


def sign(query: str, key: str) -> tuple[str, str]:
    if not key:
        return "0", "-"          # auth disabled on the device: both are ignored
    ts = str(int(time.time()))
    sig = hmac.new(key.encode(), f"{signed_payload(query)}|{ts}".encode(), hashlib.sha256).hexdigest()
    return ts, sig


# ---------------------------------------------------------------------------
# HTTP front end
# ---------------------------------------------------------------------------


class Handler(BaseHTTPRequestHandler):
    server_version = "ble-bridge/1"
    protocol_version = "HTTP/1.1"
    loop: asyncio.AbstractEventLoop
    link: Link
    verbose: bool

    def log_message(self, fmt, *args):  # quieter than the default, still useful
        if self.verbose:
            log(f"http {self.address_string()} {fmt % args}")

    def do_GET(self):  # noqa: N802 - BaseHTTPRequestHandler's name
        path, _, query = self.path.partition("?")
        if path not in ("/", ""):
            return self._reply(404, "not found\n")

        ts = self.headers.get("X-Ts", "") or "0"
        sig = self.headers.get("X-Sig", "") or "-"

        try:
            fut = asyncio.run_coroutine_threadsafe(
                self.link.request(query, ts, sig), self.loop
            )
            code, body = fut.result(timeout=self.link.timeout_s + self.link.scan_s + 5)
        except LinkError as exc:
            log(f"unreachable: {exc}")
            return self._reply(502, f"BLE unreachable: {exc}\n")
        except Exception as exc:  # noqa: BLE001
            log(f"error: {exc}")
            return self._reply(502, f"BLE error: {exc}\n")

        self._reply(code, body)

    def _reply(self, code: int, body: str):
        payload = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(payload)


# ---------------------------------------------------------------------------
# entry points
# ---------------------------------------------------------------------------


async def run_serve(args) -> int:
    link = Link(args.scan_timeout, args.timeout, args.idle,
                connect_timeout_s=args.connect_timeout)
    loop = asyncio.get_running_loop()

    handler = type("BoundHandler", (Handler,), {"loop": loop, "link": link, "verbose": args.verbose})
    httpd = ThreadingHTTPServer((args.host, args.port), handler)
    httpd.daemon_threads = True
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    log(f"serving http://{args.host}:{args.port}/ over BLE (device channel, verbatim)")

    # Let go of the link on the way out. A process that is killed while holding one
    # leaves macOS holding it too, and while *anything* is connected the device does
    # not advertise — so the next run cannot even find it. Cheap to avoid.
    stopping = asyncio.Event()
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, stopping.set)
        except NotImplementedError:  # pragma: no cover - not POSIX
            pass

    try:
        await stopping.wait()
    finally:
        log("shutting down, releasing the link")
        httpd.shutdown()
        await link.close()
    return 0


async def run_scan(args) -> int:
    """List the devices either discovery pass can see.

    Unfiltered on purpose (so a failure to match still shows what *was* heard), and
    it reports the raw advertisement fields for anything that looks like ours.
    """
    seen: dict[str, tuple] = {}

    def cb(device, adv):
        seen[device.address] = (device, adv)

    print(f"scanning {args.scan_timeout:g}s for {SERVICE_UUID} …")
    scanner = BleakScanner(detection_callback=cb)
    await scanner.start()
    try:
        await asyncio.sleep(args.scan_timeout)
    finally:
        await scanner.stop()

    hits = [(d, a) for d, a in seen.values() if looks_like_device(d, a)]
    print(f"heard {len(seen)} devices, {len(hits)} looked like usage-monitor")

    if not hits:
        print("  nothing matched.")
        print("  First check whether something is holding a link: a connected")
        print("  peripheral stops advertising, so with a bridge (or nRF Connect)")
        print("  attached the device is simply not in the air. That is not a fault,")
        print("  and --once still works then — it connects to that same link.")
        print("  Otherwise it is not advertising: check the boot log `ble:` lines")
        print("  (an advertising failure is silent apart from those).")
        for _d, a in list(seen.values())[:3]:
            print(f"    uuids={a.service_uuids} mfr={a.manufacturer_data} "
                  f"name={a.local_name!r}")
        print(f"  A matching advertisement carries mfr="
              f"{{{MFR_COMPANY:#06x}: {MFR_MAGIC.hex()}..}}")
        return 1

    for device, adv in hits:
        print(f"{device.name or adv.local_name or '(unnamed)'}  "
              f"{device.address}  rssi={adv.rssi}  mfr={adv.manufacturer_data}")
        if args.info:
            link = Link(args.scan_timeout, args.timeout, 1.0,
                        connect_timeout_s=args.connect_timeout)
            link.preset(device)
            try:
                print(f"  info: {await link.info()}")
            except LinkError as exc:
                print(f"  info: unavailable ({exc})")
            finally:
                await link.close()
    return 0


async def run_once(args) -> int:
    key = load_key(args.key)
    ts, sig = sign(args.once, key)
    # No idle disconnect here: the process exits as soon as the reply arrives, so
    # a short watchdog could only ever fire *during* the request. It did — a reply
    # slower than the 1 s threshold lost the link mid-write ("write/reply failed:
    # disconnected"), which is exactly the flakiness a one-shot cannot afford.
    # `--idle` governs the server mode only.
    link = Link(args.scan_timeout, args.timeout, args.timeout + 5,
                connect_timeout_s=args.connect_timeout)
    try:
        code, body = await link.request(args.once, ts, sig)
        if args.info:
            # Same link, one extra read: the info report carries the device's own
            # IP, which is how a caller that had to fall back to BLE learns where
            # to find it over HTTP the next time (a DHCP move on an mDNS-less
            # network is otherwise invisible to the host).
            try:
                log(f"info: {await link.info()}")
            except LinkError as exc:
                log(f"info: unavailable ({exc})")
    except LinkError as exc:
        print(f"failed: {exc}", file=sys.stderr)
        return 1
    finally:
        await link.close()
    print(f"{code} {body.rstrip()}")
    return 0 if code == 200 else 1


def main() -> int:
    p = argparse.ArgumentParser(description="BLE bridge for the ESP32 usage monitor")
    p.add_argument("--host", default=DEFAULT_HOST)
    p.add_argument("--port", type=int, default=DEFAULT_PORT)
    p.add_argument("--scan-timeout", type=float, default=DEFAULT_SCAN_S,
                   help="seconds to scan when the link is down (default %(default)s)")
    p.add_argument("--timeout", type=float, default=DEFAULT_QUERY_TIMEOUT_S,
                   help="seconds to wait for a reply notification (default %(default)s)")
    p.add_argument("--connect-timeout", type=float, default=DEFAULT_CONNECT_TIMEOUT_S,
                   help="seconds to allow a BLE connect; macOS is slow to establish one "
                        "(default %(default)s)")
    p.add_argument("--idle", type=float, default=DEFAULT_IDLE_S,
                   help="drop the link after this long with no request; a cold "
                        "reconnect costs 8-15 s against ~0.5 s warm (default %(default)s)")
    p.add_argument("--verbose", action="store_true", help="log every HTTP request")
    p.add_argument("--scan", action="store_true", help="list matching devices and exit")
    p.add_argument("--info", action="store_true",
                   help="with --scan or --once, also read the info characteristic")
    p.add_argument("--once", metavar="QUERY",
                   help="send one query (e.g. state=done, or chime=0) and exit")
    p.add_argument("--key", help="status_key for --once (default: data/config.json, then $ESP32_MONITOR_KEY)")
    args = p.parse_args()

    try:
        if args.scan:
            return asyncio.run(run_scan(args))
        if args.once:
            return asyncio.run(run_once(args))
        return asyncio.run(run_serve(args))
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
