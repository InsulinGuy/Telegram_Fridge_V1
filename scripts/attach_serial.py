#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Insulin Guy and the TeleFridge contributors.
# This file is part of TeleFridge. It comes with ABSOLUTELY NO WARRANTY and is
# NOT a medical device — see DISCLAIMER.md. See LICENSE for the full terms.
"""Attach to the serial port, avoiding a DELIBERATE reset (but see the warning).

Usage: attach_serial.py [seconds] [port] [baud]   (default 120s, 115200)

capture_boot.py pulses RTS on purpose to capture a boot from t=0. This script
does not: DTR and RTS are set false BEFORE open() so no reset is requested.

*** IT RESETS THE BOARD ANYWAY. MEASURED, NOT ASSUMED. ***

On this hardware (M5StickC USB-serial, macOS) simply opening the port resets the
ESP32 — bootloader output appears at t=0.02s every time, including with HUPCL
cleared first (`stty -f <port> -hupcl`). So this CANNOT observe a state that
already exists: a hang, a latched fault or a stuck rail is destroyed the moment
you attach, which is exactly the mistake this script was written to prevent.

Consequences worth knowing before trusting a capture:
  - A boot at the START of the log is probably YOURS, not a device event.
  - "wakeup_cause=0 first_boot=1" at t~1.6s means the attach cold-booted it and
    wiped RTC memory — including the g_diag_* counters.
  - What it CAN still measure is behaviour AFTER that first boot: repeated
    setups (a boot loop) versus one setup then silence are trivially
    distinguishable, since a loop keeps producing boots regardless of who
    caused the first one.

For genuinely non-destructive observation of a live state, the Telegram diag
footer is the only channel available — it reports RTC-backed counters without
touching the device.
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
