#!/usr/bin/env python3
"""Capture the firmware's -DFRAME_DUMP screen walk and rebuild it as PNGs.

Build with:  PLATFORMIO_BUILD_FLAGS='-DFRAME_DUMP' pio run -e esp32-usage-monitor -t upload
Then run:    python3 tools/frame_dump.py [port] [outdir]

The device streams, per screen, a line "FRAME <idx> <w> <h>\\n" followed by
w*h*2 raw little-endian RGB565 bytes and a '\\n'. Useful for checking layout
without looking at the panel.
"""
import os
import re
import struct
import sys
import time
import zlib

import serial  # pyserial


def write_png(path, w, h, rgb_rows):
    """rgb_rows: list of bytearray rows, each w*3 bytes."""
    raw = bytearray()
    for row in rgb_rows:
        raw.append(0)          # filter type 0
        raw += row
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def rgb565_to_rows(payload, w, h):
    rows = []
    for y in range(h):
        row = bytearray(w * 3)
        base = y * w * 2
        for x in range(w):
            c = payload[base + x * 2] | (payload[base + x * 2 + 1] << 8)
            r = (c >> 11) & 0x1F
            g = (c >> 5) & 0x3F
            b = c & 0x1F
            row[x * 3 + 0] = (r << 3) | (r >> 2)
            row[x * 3 + 1] = (g << 2) | (g >> 4)
            row[x * 3 + 2] = (b << 3) | (b >> 2)
        rows.append(row)
    return rows


def read_exact(ser, n, deadline):
    buf = bytearray()
    while len(buf) < n and time.time() < deadline:
        chunk = ser.read(min(65536, n - len(buf)))
        if chunk:
            buf += chunk
    return bytes(buf)


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem1101"
    outdir = sys.argv[2] if len(sys.argv) > 2 else "."
    os.makedirs(outdir, exist_ok=True)

    ser = serial.Serial(port, 115200, timeout=0.3)
    ser.setDTR(False)
    ser.setRTS(False)
    time.sleep(0.3)
    ser.reset_input_buffer()

    pending = b""
    frames = 0
    deadline = time.time() + 180
    while frames < 8 and time.time() < deadline:
        line = ser.readline()
        if not line:
            continue
        m = re.match(rb"^FRAME (\d+) (\d+) (\d+)\s*$", line.strip())
        if not m:
            if line.strip():
                print("skip:", line[:80])
            continue
        idx, w, h = int(m.group(1)), int(m.group(2)), int(m.group(3))
        payload = read_exact(ser, w * h * 2, deadline)
        if len(payload) < w * h * 2:
            print("short frame %d" % idx)
            break
        read_exact(ser, 1, deadline)   # trailing newline
        rows = rgb565_to_rows(payload, w, h)
        path = os.path.join(outdir, "frame_%d.png" % idx)
        write_png(path, w, h, rows)
        print("wrote %s (%dx%d)" % (path, w, h))
        frames += 1
    ser.close()
    if frames == 0:
        print("no frames captured (is the firmware built with -DFRAME_DUMP?)")
        sys.exit(1)


if __name__ == "__main__":
    main()
