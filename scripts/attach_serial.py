#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Insulin Guy and the TeleFridge contributors.
# This file is part of TeleFridge. It comes with ABSOLUTELY NO WARRANTY and is
# NOT a medical device — see DISCLAIMER.md. See LICENSE for the full terms.
"""Attach to the serial port WITHOUT resetting the board.

Usage: attach_serial.py [seconds] [port] [baud]   (default 120s, 115200)

capture_boot.py deliberately pulses RTS to capture a boot from t=0. That is the
wrong tool for observing a device that is ALREADY in a state you care about — a
hang, a latched fault, a stuck rail — because the reset destroys exactly the
evidence you were trying to read. This attaches passively instead: DTR and RTS
are set false BEFORE open(), so the usual auto-reset pulse never happens.

Silence is a result, not a failure: between deep-sleep wakes the device emits
nothing, so a quiet capture means it is sleeping normally rather than stuck.
"""
import sys
import time

import serial

DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 120.0
PORT = sys.argv[2] if len(sys.argv) > 2 else "/dev/cu.usbserial-614E4634A5"
BAUD = int(sys.argv[3]) if len(sys.argv) > 3 else 115200

ser = serial.Serial()
ser.port = PORT
ser.baudrate = BAUD
ser.timeout = 1
# Must be set BEFORE open() — assigning after the port is already open toggles
# the lines, which is the reset this script exists to avoid.
ser.dtr = False
ser.rts = False
ser.open()

print("attached to %s at %d baud for %.0fs (NO reset)" % (PORT, BAUD, DURATION),
      file=sys.stderr)

start = time.time()
lines = 0
while time.time() - start < DURATION:
    raw = ser.readline()
    if not raw:
        continue
    lines += 1
    print("%7.2f %s" % (time.time() - start,
                        raw.decode("utf-8", "replace").rstrip()))
    sys.stdout.flush()
ser.close()
print("captured %d lines" % lines, file=sys.stderr)
