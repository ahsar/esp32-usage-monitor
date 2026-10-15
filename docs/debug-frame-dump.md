# Debug: frame dump

Build with `-DFRAME_DUMP` to have the device walk all 7 screens, stream each raw
RGB565 canvas over USB CDC (`FRAME <i> <w> <h>` + payload) and let a host script
rebuild PNGs — useful for checking layout without looking at the panel:

```bash
PLATFORMIO_BUILD_FLAGS='-DFRAME_DUMP' pio run -e esp32-usage-monitor -t upload
python3 tools/frame_dump.py        # rebuild the streams as PNGs
```

This is how the layout (and the LVGL text pipeline) is verified without looking
at the panel. See also `docs/screens/` for the checked-in captures.
