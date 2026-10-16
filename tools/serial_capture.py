#!/usr/bin/env python3
"""Capture the ESP32 serial port, surviving chip resets / power cycles.

Usage:
  serial_capture.py [port] [seconds] [--reset-run|--jtag-reset]

The port is reopened automatically if the board resets or powers off (the USB
CDC device disappears), so a power-cycle is captured from boot #N onwards.

Note that opening the port does *not* reset this board (the S3 AMOLED 2.16 with
ARDUINO_USB_CDC_ON_BOOT), so a boot log needs one of the two reset flags:
  --reset-run   IO0 high: reboots into the application, which is what a boot log
                wants (and what pressing RESET does).
  --jtag-reset  IO0 low: reboots into the ROM download mode, i.e. it leaves the
                chip waiting to be flashed. Useful when a flash needs the board
                put into download mode without touching it.
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

# A *run*-mode reset: the same control lines, but IO0 is left high, so the chip
# boots the application instead of stopping in the ROM download mode that
# --jtag-reset lands in. This is what you want to capture a boot log: this board
# has no working "opening the port resets it" behaviour, and `pio run -t upload`
# cannot be used for it because it needs the port this tool is holding.
#
#   tools/serial_capture.py /dev/cu.usbmodem1101 25 --reset-run
if "--reset-run" in flags:
    ser.dtr = False   # IO0 high  → boot from flash
    ser.rts = True    # EN low    → hold in reset
    time.sleep(0.1)
    ser.rts = False   # EN high   → run
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
