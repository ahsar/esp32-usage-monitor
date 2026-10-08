#!/usr/bin/env python3
"""UDP audio receiver for ESP32 streaming microphone.

Usage:
    python listen.py [port]

Press Ctrl+C to exit.
WAV files are saved as recording_YYYYMMDD_HHMMSS.wav in the current directory.
"""

import socket
import struct
import sys
import time
from datetime import datetime

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 12345

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(("0.0.0.0", PORT))
sock.settimeout(1.0)

print(f"Listening on UDP port {PORT}...")
print("Hold the left button on ESP32 to start streaming.")

f = None
pcm_buf = bytearray()

def open_wav():
    global f, pcm_buf
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    name = f"recording_{ts}.wav"
    f = open(name, "wb")
    # Write placeholder WAV header (44 bytes), will fix at close
    f.write(b"RIFF" + struct.pack("<I", 0) + b"WAVE")
    f.write(b"fmt " + struct.pack("<I", 16))
    f.write(struct.pack("<HHIIHH", 1, 1, 16000, 32000, 2, 16))
    f.write(b"data" + struct.pack("<I", 0))
    pcm_buf = bytearray()
    print(f"Recording -> {name}")

def close_wav():
    global f, pcm_buf
    if f is None:
        return
    data_len = len(pcm_buf)
    # Fix RIFF size
    f.seek(4)
    f.write(struct.pack("<I", 36 + data_len))
    # Fix data chunk size
    f.seek(40)
    f.write(struct.pack("<I", data_len))
    # Write PCM data
    f.seek(44)
    f.write(pcm_buf)
    f.close()
    f = None
    dur = data_len / 32000.0
    print(f"Saved ({data_len} bytes, {dur:.1f}s)")

try:
    while True:
        try:
            data, addr = sock.recvfrom(2048)
        except socket.timeout:
            continue

        if len(data) < 4:
            continue

        # Control markers
        if data[:4] == b"STRT":
            close_wav()
            open_wav()
            continue
        elif data[:4] == b"STOP":
            close_wav()
            continue

        # Audio packet: 1 byte type + PCM data
        if data[0] == 0x01 and len(data) > 1:
            pcm = data[1:]
            if f is not None:
                pcm_buf.extend(pcm)
except KeyboardInterrupt:
    close_wav()
    print("\nExiting.")
