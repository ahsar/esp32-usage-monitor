# Power key

On the 2.16 boards the middle key is the AXP2101 `PWRON` input, so it is both
the power button and `Key1` (via a BSS138 inverter). Tap = power on; hold ≈2.5 s
= power off (`src/hw/power.cpp` sets `0x27` to ON 128 ms / OFFLEVEL 10 s /
IRQLEVEL 2.5 s, and `loop()` acts on the AXP long-press IRQ; OFFLEVEL is the
10 s hardware fallback). The press that powered the board **on** also reads as a
PWRON hold, so `loop()` consumes the IRQ the boot press produced and only acts
on a fresh one — otherwise powering on would immediately power off.
`wifi_setup.cpp` likewise no longer treats a `Key1` release as a gesture, so
letting go of the power button cannot abort the boot WiFi connect.

# Display power / heat

Rendering is **state-driven, not timer-driven**. `uiFrame()` hashes everything
the frame depends on (`uiFrameKey()` in `src/main.cpp`: page/card, quantised drag
offset, clock HH:MM, online/charging/battery, and a `s_dataRev` counter bumped on
every successful fetch) and re-renders only when that hash actually changes.
Nothing changed ⇒ no render and no panel traffic at all. Most screens hold HH:MM
(not seconds), so a static minute renders exactly once and the idle loop can run
for minutes with zero CPU drawing and zero QSPI.

Each render is a **full-frame push**: `hwDisplayPush()` (`src/hw/display.cpp`)
blits the whole 368×448 window in one QSPI transaction. We deliberately do *not*
track dirty rectangles any more — this CO5300 misbehaves when a frame is split
into many small address windows, and a dirty-rect shadow must stay byte-identical
to the panel or a single bad partial blit desynchronises it permanently (errors
then accumulate as you swipe between pages). The QSPI clock stays at the library
default 40 MHz (`BOARD_DISPLAY_QSPI_HZ`): 80 MHz intermittently wedges the bus
mid-push on this panel, so higher values need to be proven on hardware first.
Idle also dims the panel but **never blanks it** (so it stays glanceable):

- 30 s idle (`IDLE_DIM_MS`) → brightness `BRIGHT_LUT[0]` = 50/255, and it stays
  there until activity (no full screen-off stage).
- Any touch/key (and `state=running` webhooks) wakes it; the waking **tap**
  is swallowed so it doesn't also page — a wake **drag** still scrolls (the swallow
  is dropped as soon as the gesture locks an axis, `pager.cpp`).
- Pager/key/wake/data events set `s_forceRender`, so a swipe/tap never freezes on
  a mid-transition frame.
- Radio: TX power is capped at `WIFI_POWER_11dBm`. Power save is `MIN_MODEM`
  (`APP_WIFI_PS_BOOT` and `APP_WIFI_PS` in `hw/net.h`) — both while associating
  and afterwards. `MAX_MODEM` (radio dozing between beacons) was tried for idle
  power and measured out: at the same spot minutes apart, MIN_MODEM gave TLS
  handshakes of 2.2-4.4 s and all four pollers succeeded, MAX_MODEM gave
  4.5-31.7 s and every poll timed out. Re-measure `http: … tls=…` before
  switching back.

Tunables live at the top of `src/main.cpp` (search `IDLE_DIM_MS` /
`IDLE_NET_MS`) and in `data/config.json` (the `poll_*` fields). The fetch
retry/wedge policy lives in `src/poller.cpp` (see
[network-polling.md](network-polling.md)).
