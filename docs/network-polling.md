# Network polling / power

Poll periods live in `data/config.json` (`poll_usage_s`, `poll_usage_idle_s`,
`poll_weather_s`, `poll_air_s`, `poll_moon_s`). Defaults: usage 60 s, weather
30 min, air 60 min, moon 12 h. One feature toggle lives there too: `chime`
(completion cues), default `true`. `status_key` is the HMAC secret for the
status endpoint (empty = auth off). Changing `status_key` needs an `uploadfs` —
run `python3 tools/authorize.py` first, because `uploadfs` erases the device's
rotated tokens.

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
