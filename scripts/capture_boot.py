#!/usr/bin/env python3
"""Reset the ESP32 via RTS and capture serial output from t=0.

Usage: capture_boot.py [seconds] [port]   (default 120s, port auto-detected)
Bring-up aid for issue #24 debugging; safe to delete afterwards.

NOTE: an RTS reset PRESERVES RTC slow memory, so g_first_boot stays false and no
boot message is sent. To force a true cold boot on the M5StickC, hold the power
button (left side) ~6 s to make the AXP192 cut power, then press it again.
"""
import glob
import sys
import time

import serial


def default_port():
    ports = [p for p in glob.glob("/dev/cu.usbserial*") + glob.glob("/dev/cu.wchusbserial*")]
    if not ports:
        sys.exit("no /dev/cu.usbserial* found — is the board plugged in?")
    return ports[0]


DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 120.0
PORT = sys.argv[2] if len(sys.argv) > 2 else default_port()
print("capturing %.0fs from %s" % (DURATION, PORT), file=sys.stderr)

ser = serial.Serial(PORT, 115200, timeout=1)
# Auto-reset circuit: RTS drives EN (with DTR low so IO0 stays high -> normal boot).
ser.dtr = False
ser.rts = True
time.sleep(0.1)
ser.rts = False

start = time.time()
while time.time() - start < DURATION:
    line = ser.readline()
    if line:
        sys.stdout.write("%7.2f %s" % (time.time() - start, line.decode("utf-8", "replace")))
        sys.stdout.flush()
ser.close()
