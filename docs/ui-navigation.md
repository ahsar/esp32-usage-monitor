# UI / navigation

- Horizontal pages: **Weather ↔ Usage ↔ Trend ↔ Clock ↔ Notice ↔ Health**.
- Weather has vertical cards: **Now → Forecast → Sun → Air → Moon → Moonrise**.
- Both axes are **endless carousels**: dragging past the last page/card keeps
  following the finger and brings in the far end (Health ↔ Weather, Moonrise ↔
  Now), and releasing commits the wrap. There is no end-of-strip resistance and
  no snapping back at the ends.
- Drag follows the finger and snaps; tap advances; Key1 cycles. Dots: bottom row
  = pages, right column = weather cards.
- **A tap on the title bar is not a page turn**: it refreshes the visible page's
  data immediately (`forceRefreshVisible()` in `main.cpp`). The bar is where the
  Usage page already reports its data age, so cause and effect sit together, and
  the pager tap is swallowed so the page does not also advance. Rate-limited to
  one per 5 s, and it clears the job's backoff on purpose — a person asking for
  fresh numbers has decided the screen is stale.
- The two side keys (left = IO18, right = BOOT) mirror the horizontal swipe and
  **push the content the way the key sits**: left = content slides left / next
  page enters from the right, right = content slides right / previous page
  enters from the left. They are always on the page axis (even on Weather) and,
  like the swipe, wrap at the ends. Unlike Key1 they are not context-sensitive,
  and they do not dismiss the thinking animation.
- Weather cards reserve the right `WC_R` margin so content never collides with the
  card dots.

Composition lives in `src/ui_compose.{h,cpp}` (it picks the page/card(s) to draw
for the current pager state, including the two-tile swipe transition) and the
gesture/state machine in `src/pager.{h,cpp}`. The neutral page/card enums are in
`src/ui_model.h`, which is deliberately free of any drawing or hardware
dependency so `pager.cpp` can reason about pages without pulling in the display.

Text is rendered with LVGL Montserrat (anti-aliased, all copy in English) — see
[text-lvgl.md](text-lvgl.md). Text helpers take and return **logical** units.

## Usage page (and its "re-authorise" state)

Three meters — 5-hour rolling / weekly / monthly — each with a percentage, a bar,
a spend figure (`$3.20/$10.00`, left of the countdown on the same line), and a
reset countdown, drawn green under 30 %, yellow to 60 %, red above
(`src/ui/cards.cpp`, `uiDrawUsage`).

**Two pace models, one tag.** The 5-hour row's **OVER** is a real projection
(`src/util/usage_pace.h`): spend + (current rate × time to reset), where the rate
is the one the device measures from its own RAM ring (`trendBurnPerHour()`, so
this page and the Trend page cannot disagree). Only the 5-hour meter gets that
treatment — it is the only one with a measured rate.

Week/month get the *even-burn* model instead: what is spent against how much of
the window has elapsed (`elapsedPct()` from the countdown and a known window
length), with ten points of slack. It assumes spending is roughly uniform across
the window — a stated assumption, not a measurement — which is why it is a
different function rather than the 5-hour rate pushed through the same formula.
Both end in the same `OVER` tag, so the row is one word either way.

**The title bar answers "is this current?".** The status-bar dot means WiFi,
which says nothing about the last poll, and three unchanged bars look identical
in both cases. So the Usage page draws the age of the last *successful* usage
poll next to its title (`12m old`, yellow) once it passes the idle period + 60 s,
and nothing at all while the data is fresh — a note that is always there would
tick every second and repaint a panel nobody needs repainting. The threshold
uses the idle period because the device legitimately polls every three minutes
when it has been untouched; calling that stale would train the eye to ignore it.
`UiEnv::usageStale`/`usageAgeSec` carry it, and the frame key mixes the age only
while stale and only on this page (quantised to 5 minutes).

The 5-hour row can also carry an **OVER** tag (yellow, left of the countdown).
Crossing 80 % and 100 % of the rolling meter plays a cue (the "needs you" taps
and the low note, reused) exactly once per window, from the host-tested state
machine in `src/util/usage_alert.h`. A window roll-over is detected from the
meter losing spend, not from its countdown: two windows of the same length
report the same countdown. It respects the `chime` setting and, like the
re-authorise cue, only ever fires on a successful poll.

The page has one other state: when the worker has no usable
refresh token, the bars are replaced by **AUTH NEEDED** (yellow) or **AUTH
EXPIRED** (red, when the meter had data to lose) plus the way out —
`run tools/authorize.py` / `then uploadfs` — because a token the endpoint has
refused is terminal and will never fill the bars again, while three grey rows
look exactly like a fetch that is still in flight. It mirrors the Air card's
missing-`waqi_token` message.

The state comes from `fetchTaskTokensValid()` (`UiEnv::authNeeded`), which is
only false for a token refusal or a device that was never authorised — a fetch
that failed on the network leaves it true, so a flaky link cannot raise a false
alarm. The hand-off from working to dead plays the `failed` cue once (seeded
from the boot state, so an unconfigured device does not beep at every power-on).

## Weather cards (Forecast)

The second card is a three-day forecast: `Today / Tomorrow / <weekday>`, hi/lo and
the chance of rain (blue at ≥50%), from Open-Meteo (`api.open-meteo.com`) rather
than wttr.in. That is a data-source decision, not a preference: wttr.in's JSON is
~50 KB and `http_client` caps a body at 4 KB, while Open-Meteo returns just the
five daily arrays (<1 KB), needs no key, and takes the coordinates the moon API
already uses. `timezone=auto` makes each bucket the *local* day, so "Today" needs
no date math in the firmware; the weekday label comes from
`weekdayFromIsoDate()` (`src/util/time_util.h`). The WMO code → short label
mapping and the parser are in `src/parse/weather_parse.h`, host-tested; a null
rain probability is drawn as nothing rather than "0%".

## Clock page (device identity)

The Clock page is the device's own status page — the clock, the date, the battery
— so it is also where the device says **how to reach it**:

```
      192.168.20.31        ← the LAN address, or "no IP" while offline
         BLE on            ← the fallback came up (green) / did not (muted)
```

The address is packed into `UiEnv::ipv4` and mixed into `uiFrameKey()`, so a DHCP
change repaints the page on its own and the page still costs nothing when nothing
changed. It is here rather than in the status bar because that bar is per-card and
already carries the online dot, the title and the clock; and it is here rather than
behind a gesture because reading an address off a screen should not require knowing
a secret. It is also the answer to a real problem: mDNS does not cross a VLAN
boundary, so `usage-monitor.local` can simply not resolve on a segmented network,
and this saves reaching for a serial console to find the IP.

Two things to keep in mind when editing this page:

- **`uiDrawPageDots()` draws across it.** The page dots sit at `SAFE_B - 5`
  (= 211) centred, so this page's content has to stay above y≈205.
- **`BLE on` is not "advertising".** It means the peripheral was brought up
  (controller running, service registered). The BLE library keeps no advertising
  state and exposes nothing to query (its GAP handlers are empty), so a scan from
  the PC — `tools/ble-bridge.py --scan` — remains the only real test.

## Health page

The last page (`src/ui/health.cpp`) is the device describing itself: WiFi (or
`down`), uptime, free heap (internal / PSRAM), the outcome and age of the last
fetch, that fetch's DNS and TLS timings, the firmware version, and a footer line
for the two booleans (`token ok • BLE on`). It exists because all of that used to
be visible only over USB CDC — `diag`'s periodic line and `tools/link_probe.py` —
which is exactly the cable you do not want to reach for when the panel looks
stale. It is read-only by construction: nothing on the page starts a fetch,
writes flash or touches the radio.

Data comes from `main.cpp`'s `refreshEnv()` (the existing 1 Hz tick) filling a
`HealthData` (`src/data_types.h`), deliberately **not** from `src/diag.*`, which
is documented as temporary. The two HTTP phase times are a best-effort
cross-task read of single 32-bit words (`httpLastTimings()`).

One layout note and one rendering note:

- Page dots at `SAFE_B - 5` (= 211): content stays above y≈205, like Clock.
- **The frame key is quantised for this page.** Health is the only page whose
  content moves on its own, so `uiFrameKey()` mixes its fields in — but only while
  Health is the visible page, and only after rounding (uptime to the minute, rssi
  to 5 dBm, heap to 4 KB/64 KB, fetch age to 10 s). Raw values would mean a
  full-frame push every second; the quantisation keeps the "a static minute costs
  zero renders" property that pays for the animation screens.

## Trend page

The Trend page (`src/ui/trend.cpp`) plots the rolling (5-hour) meter as cost:
headline spend, `$/h` burn rate, time-to-limit, a cumulative curve and per-poll
bars. It reads a ring (`src/usage_history.*`) that `serviceFetch()` appends to on
each successful `FETCH_USAGE`. The pure math lives in `src/util/trend_util.h`
(microcents → `$`, burn rate, ETA) and is host-tested. `UsageData` carries the
`usedMicroCents` / `limitMicroCents` fields the console sends per meter.

**The ring now survives a reboot.** It samples the rolling window, whose
timestamps are boot-relative `millis()` — meaningless after a restart — so
`src/trend_store.cpp` stores wall-clock epochs in `/trend.txt` and owns the
translation: at boot `trendStoreRestore()` keeps only samples that could still
belong to the current 5-hour window and rebases them onto this boot's timeline
(`trendNowSec()`), and `trendPush()` writes on that same virtual clock so a new
sample lands exactly one poll interval after the last restored one. Writes are
coalesced (at most every 5 minutes) and also flushed into the power-off and OTA
paths, because flash writes belong on the loop task. A missing/corrupt/unclocked
file just means the page collects from scratch, which is what it always did.
The line format is in `src/util/trend_store_util.h` and host-tested.

## Notice page

The second-to-last page, between Clock and Health, and usually empty:

```
        MEETING            ← kind, upper-cased (dim)
         12 min            ← countdown to the event (F_MED); "now" in amber
   Standup in the big room ← the pushed text, wrapped to 3 lines
```

`?notice=1&kind=…&text=…&ttl=…[&left=…]` (`src/parse/notice_parse.h`) is the one
remote string that *lingers*: it stays until the TTL runs out, which is what
makes "the next meeting is in 8 minutes" useful without the PC re-sending every
minute. The countdown is to the event (`left`), not to the expiry, and the device
ticks it locally; `?notice=1&clear=1` drops it now. Both are signed over the whole
query and require `status_key` in the same way `?agent=` does (bench mode when
there is no key — it is ephemeral, unlike settings).

Two rules follow from "lingers", and both are enforced in the parser, not trusted
from the client: **a TTL is mandatory** (an unexpiring notice is a lie waiting to
happen), and the text goes through the *same* sanitiser and field caps as
`?agent=` — printable ASCII only, sized so a fully percent-encoded request still
fits `STATUS_QUERY_MAX`. The card clips to the panel width itself rather than
trusting the client, exactly like the agent screens. Arriving does **not** wake
the panel: the display is only ever dimmed, so a reminder is readable when you
look without a chatty producer keeping the device awake. A notice expiring bumps
its revision, which is what repaints the page back to "no notice".

## No IMU / gestures

There is **no IMU/gesture handling**: the QMI8658 is not initialised and
`src/gestures.*` / `src/hw/imu.*` / `src/util/gesture_util.h` are gone. Shake,
double-tap, pickup, face-down blanking and gyro page-stepping were removed
deliberately — the face-down path was the only thing that could blank the panel
while the firmware kept running, and a false positive was indistinguishable from
a dead device. Navigation is touch + the keys (Key1 cycles; the left/right side
keys step the page axis); the panel is only ever dimmed (never blanked) when
idle.
