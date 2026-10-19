# Debug: frame dump

Build with `-DFRAME_DUMP` to have the device walk the screens, stream each raw
RGB565 canvas over USB CDC (`FRAME <i> <w> <h>` + payload) and let a host script
rebuild PNGs — useful for checking layout without looking at the panel:

```bash
PLATFORMIO_BUILD_FLAGS='-DFRAME_DUMP' pio run -e esp32-usage-monitor -t upload
python3 tools/frame_dump.py        # rebuild the streams as PNGs
```

This is how the layout (and the LVGL text pipeline) is verified without looking
at the panel. See also `docs/screens/` for the checked-in captures.

The five **WiFi setup screens** (boot connect, scanning, list, password, result)
are dumped first, as frames 10-14, from a seeded scan
(`wifiSetupDebugSeed()`, six names including a deliberately over-long one,
because an SSID's length is not ours to choose). They are otherwise unreachable:
`loop()` returns early while the setup screen is open, so the page walk below it
never runs. The walk seeds its own state rather than borrowing the device's, so
it runs whether or not the device is on the air — otherwise these frames could
only ever be captured from a device left without credentials. Frames 0-6 keep their usual meaning — the main pages and weather
cards, which need real poll data (offline, the walk uses placeholders after 60 s).
