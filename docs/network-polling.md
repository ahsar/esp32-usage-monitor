# Network polling / power

Poll periods live in `data/config.json` (`poll_usage_s`, `poll_usage_idle_s`,
`poll_weather_s`, `poll_air_s`, `poll_moon_s`). Defaults: usage 60 s, weather
30 min, air 60 min, moon 12 h. One feature toggle lives there too: `chime`
(completion cues), default `true`. `status_key` is the HMAC secret for the
status endpoint (empty = auth off). Changing `status_key` needs an `uploadfs` —
run `python3 tools/authorize.py` first, because `uploadfs` erases the device's
rotated tokens (the pre-hook in `tools/uploadfs_guard.py` will insist on it).

The schedule and the failure policy are **pure logic in `src/poller.{h,cpp}`**
(timers, exponential backoff, the wedge ladder) and are covered by
`test/test_poller/`. `main.cpp` only applies the config in `setup()`
(`pollerInit`) and asks `pollerSelect()` for the next job each iteration.

- **Idle backoff.** When the device has been untouched for `IDLE_NET_MS` (30 s),
  the usage poll stretches to `poll_usage_idle_s` (180 s) — the meter only matters
  while someone is looking. Any activity resets it; `screenWake()` also forces one
  usage fetch if the meter has gone >30 s stale (rate-limited so a chatty status
  webhook can't spam fetches).
- **One job in flight.** `pollerSelect()` returns the most overdue poller
  (usage → weather → moon → air) that is not cooling down and whose gate is
  satisfied (a usable OAuth token for usage, a configured waqi token for air);
  `serviceFetch()` harvests the result before the next job is scheduled, so a
  burst of due timers (all four after a long idle) drains one at a time. A timer
  is only marked run when `fetchTaskStart()` accepts the job, so a busy worker
  just retries a later iteration. A worker that stops making progress is
  restarted by the watchdog instead of freezing every poller — see
  [threading-fetch.md](threading-fetch.md).
- **A failure retries on the backoff, not on the interval.** `pollerResult()`
  sets the cooldown *and* re-arms that job's timer, so the next attempt is 60 s
  out (doubling to a 16 min ceiling) rather than one interval out. The two have
  to agree because the intervals differ by two orders of magnitude: without the
  re-arm a single blip cost 30 minutes of missing weather, 60 minutes of air and
  12 h of moon, while the 60 s usage meter repaired itself in a minute — the
  report that motivated it was "weather is gone, opencode is fine", and the only
  fast fix was a reconnect (`pollerForceAll()`). wttr.in and MET Norway also
  retry three times *inside* the job, since their failures are the cheap
  transient kind (a 503 burst, an empty body); the cooldown still gates every
  one of those, so a dead endpoint cannot hot-loop.
- **AP re-selection (roaming).** Offices put many APs on one SSID, and an ESP32
  has no 802.11k/v/r: it stays on the BSSID it picked until the link dies. The
  failure this exists for is an AP that *keeps* the link alive and carries
  nothing — measured on 2026-10-15, a distant AP where every fetch sat in the TLS
  handshake until the 30 s cap while `online=1 wifi=3` and the rssi stayed inside
  the good AP's band (see [power.md](power.md)). So the trigger is transport
  health, not rssi: `serviceFetch()` passes `!httpTransportFailed()` into
  `pollerResult()`, and `ROAM_FAILS` (3) consecutive dead-path fetches **with no
  success in between** — a 5xx proves the AP is carrying traffic, so it does not
  count — make `pollerRoamDue()` true. `loop()` then calls `netRoam()`, which
  scans while associated and re-associates pinned to the strongest *other* BSSID
  of our SSID (`WiFi.begin(ssid, pass, channel, bssid)`), falling back to
  un-pinned selection if that AP has vanished. `pollerRoamAttempted()` clears the
  streak and holds the next attempt off for `ROAM_COOLDOWN_MS` (3 min) whether or
  not the station moved, so a scan that finds nothing better cannot turn into a
  scan per iteration. The whole trip costs loop() ~2-8 s (scan + reassociation);
  it only runs while the fetch worker is idle. The accepted false positive is a
  *WAN* outage: every host fails for a reason no AP can fix, so the station may
  hop once per cooldown until the upstream returns — a few seconds of disruption
  each time, bounded by the cooldown, and cheaper than guessing.

  Verified against that AP on 2026-10-15: three fetches died at the 30 s cap
  (`opencode.ai`, `wttr.in`, `api.met.no`), `netRoam()` fired, re-associated in
  **656 ms** to a third BSSID on the same SSID, and the next two fetches
  succeeded — air at +10 s, usage at +87 s. The same board had previously sat on
  that AP for a 4.5-minute sample without ever leaving it.

# WiFi scan

`netScan()` (`src/hw/net.cpp`) scans with the station kept up: it aborts any
in-flight connect with a raw `esp_wifi_disconnect()`, turns power save **off**
for the sweep, and uses a 150 ms/channel active scan (~2 s). The old
`WIFI_OFF → WIFI_STA` power-cycle ×3 re-ran WiFi bring-up + RF calibration each
time and, with modem sleep on, stretched a boot scan past 30 s.

On boot with saved credentials the setup UI shows a **30 s connect grace**
(`wifi_setup.cpp`, `CONNECT_GRACE_MS`) before falling back to a scan, so a
slow-but-good connect is no longer aborted by a scan. Tap or press a key to
skip straight to the network list.
