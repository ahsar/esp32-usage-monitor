# UI / navigation

- Horizontal pages: **Weather ↔ Usage ↔ Trend ↔ Clock**.
- Weather has vertical cards: **Now → Sun → Air → Moon → Moonrise**.
- Drag follows the finger and snaps; tap advances; Key1 cycles. Dots: bottom row
  = pages, right column = weather cards.
- Weather cards reserve the right `WC_R` margin so content never collides with the
  card dots.

Composition lives in `src/ui_compose.{h,cpp}` (it picks the page/card(s) to draw
for the current pager state, including the two-tile swipe transition) and the
gesture/state machine in `src/pager.{h,cpp}`. The neutral page/card enums are in
`src/ui_model.h`, which is deliberately free of any drawing or hardware
dependency so `pager.cpp` can reason about pages without pulling in the display.

Text is rendered with LVGL Montserrat (anti-aliased, all copy in English) — see
[text-lvgl.md](text-lvgl.md). Text helpers take and return **logical** units.

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
a dead device. Navigation is touch + Key1 only; the panel is only ever dimmed
(never blanked) when idle.
