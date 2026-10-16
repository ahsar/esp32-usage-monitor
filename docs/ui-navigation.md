# UI / navigation

- Horizontal pages: **Weather ↔ Usage ↔ Trend ↔ Clock**.
- Weather has vertical cards: **Now → Sun → Air → Moon → Moonrise**.
- Both axes are **endless carousels**: dragging past the last page/card keeps
  following the finger and brings in the far end (Clock ↔ Weather, Moonrise ↔
  Now), and releasing commits the wrap. There is no end-of-strip resistance and
  no snapping back at the ends.
- Drag follows the finger and snaps; tap advances; Key1 cycles. Dots: bottom row
  = pages, right column = weather cards.
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

## Trend page

The Trend page (`src/ui/trend.cpp`) plots the rolling (5-hour) meter as cost:
headline spend, `$/h` burn rate, time-to-limit, a cumulative curve and per-poll
bars. It reads a **RAM-only** ring (`src/usage_history.*`) that `serviceFetch()`
appends to on each successful `FETCH_USAGE`; nothing is persisted, so a reboot
just restarts the history. The pure math lives in `src/util/trend_util.h`
(microcents → `$`, burn rate, ETA) and is host-tested. `UsageData` carries the
`usedMicroCents` / `limitMicroCents` fields the console sends per meter.

## No IMU / gestures

There is **no IMU/gesture handling**: the QMI8658 is not initialised and
`src/gestures.*` / `src/hw/imu.*` / `src/util/gesture_util.h` are gone. Shake,
double-tap, pickup, face-down blanking and gyro page-stepping were removed
deliberately — the face-down path was the only thing that could blank the panel
while the firmware kept running, and a false positive was indistinguishable from
a dead device. Navigation is touch + the keys (Key1 cycles; the left/right side
keys step the page axis); the panel is only ever dimmed (never blanked) when
idle.
