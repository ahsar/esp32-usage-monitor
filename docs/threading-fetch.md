# Blocking fetches never freeze the UI

`loop()` is single-threaded and the network fetches are **blocking** (up to
8–12 s each, wttr.in and MET Norway retried up to ×3), so they run on their own
FreeRTOS task —
`src/fetch_task.{h,cpp}`. The previous design ran them inline and pumped the UI
from inside the request (`httpSetUiPump`), but that could only cover the
response-read loop: `client.connect()` (DNS + TCP + TLS handshake) has no
callback to pump from, so the loop froze for seconds. It froze *exactly* on
wake, because `screenWake()` forces a usage fetch the moment a finger lands —
and after a long idle all four pollers are due back to back.

- `loop()` only **schedules** (`fetchTaskStart()`, one job in flight) and
  **harvests** (`serviceFetch()` → `fetchTaskHarvest()`), copying results into
  `s_usage/s_weather/...` and bumping `s_dataRev` on the main task. The fetch
  task never touches UI state, the display or I2C, and runs at the loop task's
  priority so FreeRTOS time-slices input/render against the TLS handshake.
- Jobs carry a `FetchSeed` (the data currently on screen) so a failed poll
  still lands: the fetcher flags weather/moon/air `stale` against real values
  and `serviceFetch()` republishes them without bumping `s_dataRev` — the same
  behaviour as the old in-place mutation.
- The fetch task owns the OAuth `TokenSet` (refresh included). LittleFS is not
  thread-safe and is owned by `loop()`, so rotated tokens are handed back via
  `fetchTaskPendingSave()` and written to flash by `serviceFetch()`.
- `handleHttpStatus()` is now called only from `loop()` (every ~16 ms), i.e.
  *more* often than the old 100 ms in-request pump.
- `uiFrame(now)` (`main.cpp`) still owns activity/idle, Key1, the pager, render
  and push, and runs every iteration; the pager also runs while the
  `state=running` animation owns the screen.

Each job logs `fetch <name>: ok/fail in <ms>` — on the old inline design that
duration was visible UI freeze, now it is only worker time. The first job also
prints the task's stack headroom (16 KB reserved for mbedTLS). Each HTTPS
request additionally logs its phases — `http: <host> -> <status> ip=… dns=…
tls=… read=… rx=…` — so a slow host is obvious at a glance, and the 10 s
periodic diag line carries the in-flight job, its age and the worker's current
breadcrumb (`job=… age=… bc=… host=…`).

**Every blocking phase is bounded** (`src/http_client.cpp`): the name is
resolved first with its own 8 s deadline (the TLS connect resolves internally
via `lwip_getaddrinfo`, which takes no timeout and would otherwise hide DNS in
the connect phase), the handshake is capped at `HANDSHAKE_TIMEOUT_S` and the
read loop runs for at most `timeoutMs`. **`setHandshakeTimeout()` takes
seconds** — it multiplies by 1000 internally and the value is compared against
`millis()`; passing milliseconds asked for 4 h 10 m instead of 15 s, which is
what once froze every poller for hours. Worst case per request is
8 s + 10 s + 30 s + `timeoutMs`.

## Two safety nets

The policy lives in `src/poller.cpp` (host-tested — see `test/test_poller/`);
`main.cpp` only acts on it.

- **Per-job cooldown.** A failed fetch backs its host off (exponential from
  60 s, capped at 2 min for usage and 16 min for weather/moon/air) *and* re-arms
  that job's own timer, so the cooldown — not the interval — is what paces the
  retry. That agreement is load-bearing for the long-interval jobs: weather polls
  every 30 min and air every 60, so with the timer left alone the backoff was
  dead code and a single failure cost a whole interval of missing data.
- **Wedge watchdog.** A job older than `JOB_DEADLINE_MS` (4 min) means the
  worker is stuck inside an SDK call no timeout covers, so `fetchTaskRestart()`
  recreates the worker (its tokens survive — they are file-scope statics) and
  the offending host is paused 5 min (30 min if it recurs). The abandoned TLS
  session leaks, which the pause bounds.
