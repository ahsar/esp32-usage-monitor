#!/usr/bin/env python3
"""Measure the device's link quality from its own logs.

The radio policy in `src/main.cpp` / `docs/power.md` is only as good as the
`http: … tls=…` numbers behind it, and those go stale the moment the device
moves to another AP. This samples the USB serial for a window (optionally
forcing extra usage fetches through the status endpoint, which is how you get
more than one attempt per minute), then reports what actually happened.

    python3 tools/link_probe.py --minutes 10                        # passive
    python3 tools/link_probe.py --cycles 10                         # reset-per-sample density
    python3 tools/link_probe.py --minutes 10 --drive 172.16.29.91   # + one nudge
    python3 tools/link_probe.py --analyze /tmp/link-probe-123.log   # existing log

Polling is 60 s while the device is active and 180 s once it idles out, so a
passive window is mostly a way to watch an rssi trend rather than to gather
attempts — reach for `--cycles` when you need a failure rate. Two caveats on
that mode: every sample is a cold association, and the reset window censors any
attempt slower than `--cycle-seconds` (25 s covers a healthy 4-job boot), so
treat its failure count as a lower bound and read the `tls=` medians with it.

`--drive` needs the device to be reachable: `usage-monitor.local` works from the
same L2, otherwise use the IP (mDNS does not cross subnets). The status endpoint
answers unsigned requests while `status_key` is empty (see docs/pc-bridge.md).

Reading the serial port resets the board (USB CDC), which is fine — it just
restarts the pollers — but it does interrupt whatever fetch is in flight, so the
first attempt after the port opens is not counted.
"""
import argparse
import pathlib
import re
import statistics
import sys
import time
import urllib.request

FIELDS = {
    "ok": re.compile(r"^http: (\S+) -> (\d+) ip=\S+ dns=(\d+)ms tls=(\d+)ms read=(\d+)ms rx=(\d+)$"),
    "headers": re.compile(r"^http: (\S+) no response headers ip=\S+ dns=(\d+)ms tls=(\d+)ms read=(\d+)ms rx=(\d+)$"),
    "connect": re.compile(r"^http: connect failed (\S+) ip=\S+ dns=(\d+)ms tls=(\d+)ms \((.*)\)$"),
    "dns": re.compile(r"^http: (\S+) dns failed/timed out after (\d+)ms$"),
    "fetch": re.compile(r"^fetch (\w+): (ok|fail) in (\d+)ms"),
    "boot": re.compile(r"^diag: ip=(\S+) rssi=(-?\d+)(?: bssid=(\S+))?"),
    # The periodic heartbeat. rssi= is only there on firmware that carries it.
    "tick": re.compile(r"online=(\d) wifi=(\d)(?: rssi=(-?\d+))?"),
}


def parse(path):
    """Pull the interesting events out of a serial log.

    Every HTTP attempt is normalised to (host, kind, tlsMs, detail, bssid) while
    walking the log in order, so each attempt carries the AP it went out on —
    without that tag, a window that roamed between APs invites a comparison
    between two different links.
    """
    events = {"attempts": [], "fetch": [], "rssi": [], "boots": [],
              "offline": 0, "ticks": 0, "bootsWithTraffic": set()}
    bssid = "?"
    boot = 0
    for line in pathlib.Path(path).read_text(errors="replace").splitlines():
        if m := FIELDS["boot"].match(line):
            bssid = m[3] or "?"
            boot += 1
            events["boots"].append((int(m[2]), bssid))
            events["rssi"].append(int(m[2]))
            continue
        if m := FIELDS["ok"].match(line):
            events["attempts"].append(
                (m[1], "ok", int(m[4]), f"HTTP {m[2]}, read {m[5]}ms, rx {m[6]}B", bssid))
            events["bootsWithTraffic"].add(boot)
            continue
        if m := FIELDS["headers"].match(line):
            events["attempts"].append(
                (m[1], "no-headers", int(m[3]), f"read {m[4]}ms, rx {m[5]}B", bssid))
            events["bootsWithTraffic"].add(boot)
            continue
        if m := FIELDS["connect"].match(line):
            events["attempts"].append((m[1], "connect", int(m[3]), m[4], bssid))
            events["bootsWithTraffic"].add(boot)
            continue
        if m := FIELDS["dns"].match(line):
            events["attempts"].append(
                (m[1], "dns", None, f"gave up after {m[2]}ms", bssid))
            events["bootsWithTraffic"].add(boot)
            continue
        if m := FIELDS["fetch"].match(line):
            events["fetch"].append((m[1], m[2], int(m[3])))
            continue
        if m := FIELDS["tick"].search(line):
            events["ticks"] += 1
            if m[1] == "0":
                events["offline"] += 1
            if m[3]:
                events["rssi"].append(int(m[3]))
    return events


def pct(part, whole):
    return f"{100 * part / whole:.0f}%" if whole else "n/a"


def attempts_of(events):
    """Every HTTP attempt as (host, kind, tlsMs, detail, bssid).

    `tlsMs` is None for a DNS failure, which never got to a handshake — mixing
    the failure shapes as raw tuples is how the first version of this report
    ended up sorting an error string against a millisecond count.
    """
    return events["attempts"]


def report(events, source):
    rows = attempts_of(events)
    ok = [r for r in rows if r[1] == "ok"]
    print(f"link probe — {source}")
    if not rows and not events["fetch"]:
        print("  no traffic in the window (device idle, off, or the log is empty)")
        return
    print(f"  http attempts {len(rows)}  ok {len(ok)} ({pct(len(ok), len(rows))})  "
          f"failed {len(rows) - len(ok)} ({pct(len(rows) - len(ok), len(rows))})")
    boots, served = len(events["boots"]), len(events["bootsWithTraffic"])
    if boots and served < boots:
        print(f"  {boots - served}/{boots} boots produced no request at all — not "
              f"associated yet, or the capture window closed first (this is why a "
              f"failure count here is a lower bound)")

    hosts = {}
    for row in rows:
        hosts.setdefault(row[0], []).append(row)
    for host, host_rows in sorted(hosts.items()):
        tls = sorted(r[2] for r in host_rows if r[2] is not None)
        stats = f"tls {tls[0]}-{tls[-1]}ms (med {statistics.median(tls):.0f}ms)" if tls else "no handshake"
        host_ok = len([r for r in host_rows if r[1] == "ok"])
        print(f"    {host:<18} {host_ok}/{len(host_rows)} ok   {stats}")

    if len({r[4] for r in rows}) > 1:
        print("  by AP (a window that roamed compares two different links)")
        aps = {}
        for row in rows:
            aps.setdefault(row[4], []).append(row)
        for bssid, ap_rows in sorted(aps.items()):
            ap_ok = len([r for r in ap_rows if r[1] == "ok"])
            tls = sorted(r[2] for r in ap_rows if r[2] is not None)
            med = f"med tls {statistics.median(tls):.0f}ms" if tls else "no handshake"
            print(f"    {bssid:<20} {ap_ok}/{len(ap_rows)} ok   {med}")

    if len(ok) != len(rows):
        print("  failures")
        for host, kind, tls, detail, _ in rows:
            if kind != "ok":
                print(f"    {kind:<10} {host:<18} "
                      f"{'' if tls is None else f'tls={tls}ms  '}{detail}")

    jobs = {}
    for job, verdict, ms in events["fetch"]:
        entry = jobs.setdefault(job, [0, 0, []])
        entry[0 if verdict == "ok" else 1] += 1
        entry[2].append(ms)
    for job, (n_ok, n_fail, times) in sorted(jobs.items()):
        print(f"  fetch {job:<8} {n_ok} ok / {n_fail} failed   "
              f"completed in {min(times)}-{max(times)}ms")

    if events["rssi"]:
        rssi = sorted(events["rssi"])
        print(f"  rssi {rssi[0]}..{rssi[-1]} dBm (med {statistics.median(rssi):.0f}, "
              f"n={len(rssi)})  offline ticks {events['offline']}/{events['ticks']}")
    bssids = {}
    for _, bssid in events["boots"]:
        bssids[bssid] = bssids.get(bssid, 0) + 1
    if len(bssids) > 1:
        print("  WARNING: associated with more than one AP — roaming between them "
              "confounds any before/after comparison")
    for bssid, n in sorted(bssids.items(), key=lambda kv: -kv[1]):
        print(f"    ap {bssid}  x{n}")


def capture(path, port, minutes, drive, interval):
    """Read the serial port for the window, driving fetches if asked."""
    import serial  # pyserial is only needed for capture, not for --analyze

    stop_at = time.time() + minutes * 60
    next_poke = time.time()
    ser = serial.Serial(port, 115200, timeout=0.3)
    ser.dtr = False
    ser.rts = False
    print(f"sampling {port} for {minutes} min -> {path}"
          + (f"  (poking {drive} every {interval}s)" if drive else ""))
    with open(path, "w") as out:
        while time.time() < stop_at:
            data = ser.read(8192)
            if data:
                out.write(data.decode("utf-8", "replace"))
                out.flush()
            if drive and time.time() >= next_poke:
                next_poke = time.time() + interval
                try:
                    urllib.request.urlopen(
                        f"http://{drive}/?state=running", timeout=5).read()
                except OSError as err:
                    print(f"  (drive {drive} failed: {err})")
    ser.close()
    if drive:
        # Leave the panel out of the "Thinking…" animation. NOTE the pokes above
        # are near-useless for density: status_server's enterActive() returns
        # early when the state is unchanged, so only the first one reaches the
        # wake callback. Use --cycles for density instead.
        try:
            urllib.request.urlopen(f"http://{drive}/?state=done", timeout=5).read()
        except OSError:
            pass


def capture_cycles(path, port, cycles, seconds):
    """Density mode: one reset per sample.

    Boot forces all four pollers (`pollerForceAll()` in main.cpp), so a cycle buys
    ~4 attempts plus an rssi/BSSID line in ~25 s, instead of waiting out 60 s
    idle-paced polls. It re-associates every cycle, so this measures "what a cold
    boot gets", not the steady state — which is also what makes it good at
    exposing a bad AP choice.

    NOTE: the reset this relies on does **not** happen on the native-USB boards
    (the S3 AMOLED 2.16 with ARDUINO_USB_CDC_ON_BOOT). DTR/RTS are deasserted
    below and nothing else pulls EN, so the board keeps running and measured `t=`
    climbing straight through a `serial_capture.py` session. Treat this mode as
    "N samples", not "N cold boots", until a reset that actually works is wired in
    (esptool's `--after hard_reset`, or pressing the button).
    """
    import serial

    print(f"cycling {port} x{cycles} ({seconds}s each) -> {path}")
    with open(path, "a") as out:
        for i in range(cycles):
            ser = serial.Serial(port, 115200, timeout=0.3)
            ser.dtr = False
            ser.rts = False
            until = time.time() + seconds
            while time.time() < until:
                data = ser.read(8192)
                if data:
                    out.write(data.decode("utf-8", "replace"))
                    out.flush()
            ser.close()
            print(f"  cycle {i + 1}/{cycles}")
            time.sleep(2)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--minutes", type=float, default=10, help="capture window (default 10)")
    ap.add_argument("--port", default="/dev/cu.usbmodem1101", help="USB serial device")
    ap.add_argument("--cycles", type=int, help="density mode: N reset-per-sample cycles")
    ap.add_argument("--cycle-seconds", type=float, default=25,
                    help="how long to listen after each reset (default 25)")
    ap.add_argument("--drive", help="host/IP to force usage fetches through (status endpoint)")
    ap.add_argument("--interval", type=float, default=33,
                    help="seconds between forced fetches (default 33, the staleness gate)")
    ap.add_argument("--log", help="where to write the raw capture")
    ap.add_argument("--analyze", help="report on an existing log instead of capturing")
    args = ap.parse_args()

    if args.analyze:
        report(parse(args.analyze), args.analyze)
        return 0

    path = args.log or str(pathlib.Path("/tmp") / f"link-probe-{int(time.time())}.log")
    if args.cycles:
        pathlib.Path(path).unlink(missing_ok=True)
        capture_cycles(path, args.port, args.cycles, args.cycle_seconds)
    else:
        capture(path, args.port, args.minutes, args.drive, args.interval)
    print()
    report(parse(path), path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
