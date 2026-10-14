#!/usr/bin/env python3
"""Capture the ESP32 serial port, surviving chip resets / power cycles.

Usage:
  serial_capture.py [port] [seconds] [--jtag-reset|--no-reset]

The port is reopened automatically if the board resets or powers off (the USB
CDC device disappears), so a power-cycle is captured from boot #N onwards.
"""
import sys
import time
import serial

args = [a for a in sys.argv[1:] if not a.startswith("--")]
flags = [a for a in sys.argv[1:] if a.startswith("--")]

port = args[0] if len(args) > 0 else "/dev/cu.usbmodem1101"
secs = float(args[1]) if len(args) > 1 else 30.0

t0 = time.time()
buf = b""


def emit(text):
    print(f"[{time.time() - t0:8.2f}] {text}")
    sys.stdout.flush()


def open_port():
    deadline = time.time() + 30
    while time.time() < deadline:
        try:
            return serial.Serial(port, 115200, timeout=0.2)
        except Exception:
            time.sleep(0.2)
    return None


ser = open_port()
if ser is None:
    emit(f"cannot open {port}")
    sys.exit(1)

if "--jtag-reset" in flags:
    ser.setRTS(False); ser.setDTR(False)
    time.sleep(0.1)
    ser.setDTR(True); ser.setRTS(False)
    time.sleep(0.1)
    ser.setRTS(True); ser.setDTR(False); ser.setRTS(True)
    time.sleep(0.1)
    ser.setDTR(False); ser.setRTS(False)
    time.sleep(0.05)

while time.time() - t0 < secs:
    try:
        data = ser.read(8192)
    except Exception as e:
        emit(f"[port lost: {e} — reopening]")
        try:
            ser.close()
        except Exception:
            pass
        ser = open_port()
        if ser is None:
            emit("[port still gone — will keep trying]")
            time.sleep(1)
            continue
        emit("[port reopened]")
        continue
    if data:
        buf += data
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            emit(line.decode("utf-8", "replace").rstrip())
if buf:
    emit(f"[pending ] {buf.decode('utf-8', 'replace').rstrip()}")
try:
    ser.close()
except Exception:
    pass
