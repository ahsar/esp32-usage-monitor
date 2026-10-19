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

Shutting down is also the last chance to persist a token the background worker
rotated: `hwPowerOff()` cuts the rails with no reboot, so a rotation still sitting
in the fetch task's hand-off slot would simply die, and the next boot would replay
a refresh token the server has already consumed — which revokes the whole family.
`loop()` therefore runs the same flush OTA uses (`flushPendingTokens()`) *after*
the "Shutting down…" banner's delay and immediately before the power-off: the
device is still polling for those 400 ms, and the flush has to be the last thing
that happens rather than the first. The worker also publishes a rotation the
moment the endpoint rotates it (not at the end of the job), which is what keeps
the unpersisted window down to the round trip.

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
- Radio. TX power is `APP_WIFI_TX_DBM` (`hw/net.h`; default `WIFI_POWER_11dBm`
  ≈ 13 mW, against ~89 mW at the 19.5 dBm maximum). It is a *site* knob: the AP
  is the one that has to hear the device, and the rssi the device prints only
  describes the downlink. Power save is `MIN_MODEM` while idle
  (`APP_WIFI_PS_BOOT` / `APP_WIFI_PS`), and the station is taken out of modem
  sleep for the duration of a fetch (`netRadioWake()` / `netRadioRest()`) — the
  same shape as `netScan()` and the OTA transfer window. `MAX_MODEM` (radio
  dozing between beacons) was tried for idle power and measured out: at the same
  spot minutes apart, MIN_MODEM gave TLS handshakes of 2.2-4.4 s and all four
  pollers succeeded, MAX_MODEM gave 4.5-31.7 s and every poll timed out.
  Re-measure `http: … tls=…` before switching back.
- What that measurement looks like: `tools/link_probe.py --cycles` resets the
  board once per sample, and since boot forces all four pollers that buys four
  jobs plus an `rssi=`/`bssid=` line per boot. A 2026-10-15 run on a congested AP
  found the dropouts were **not** the radio. The same SSID carries two BSSIDs,
  and on `4C:D0:DD:14:7C:50` the device associated fine (`online=1 wifi=3`, DHCP
  leased, rssi -66…-73) but never completed a TCP/TLS connect: the usage job sat
  in `fbc=http-connect` until the 30 s cap, so the pollers queued behind it never
  ran and the panel simply went stale. It stuck to that AP for 10/10 cycles,
  while the `E4:DC:CC:FF:62:B0` cycles in the same window fetched all four jobs
  in 3-4 s. Read the boot line's `bssid=` before blaming the firmware. On the
  good AP that sample measured opencode.ai `tls` at a 3043 ms median with 3/26
  attempts failing (18.6 s EOF; 11.6 s with zero response bytes) against 2159 ms
  and 0/24 with the radio awake for the fetch window — one arm each, at different
  times, so re-measure rather than trust it.

Tunables live at the top of `src/main.cpp` (search `IDLE_DIM_MS` /
`IDLE_NET_MS`) and in `data/config.json` (the `poll_*` fields). The fetch
retry/wedge policy lives in `src/poller.cpp` (see
[network-polling.md](network-polling.md)).
