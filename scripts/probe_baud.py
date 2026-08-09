#!/usr/bin/env python3
"""Reset the board and sample the serial stream at each candidate baud rate,
scoring how much of it decodes as printable ASCII. Highest score wins."""
import sys
import time

import serial

PORT = "/dev/cu.usbserial-614E4634A5"
RATES = [115200, 230400, 460800, 921600]

for rate in RATES:
    try:
        ser = serial.Serial(PORT, rate, timeout=0.5)
    except Exception as exc:  # noqa: BLE001
        print("%7d  OPEN FAILED: %s" % (rate, exc))
        continue
    ser.dtr = False
    ser.rts = True
    time.sleep(0.1)
    ser.rts = False
    time.sleep(0.4)

    raw = b""
    end = time.time() + 3.0
    while time.time() < end:
        raw += ser.read(4096)
    ser.close()

    if not raw:
        print("%7d  no data" % rate)
        continue
    printable = sum(1 for b in raw if 32 <= b < 127 or b in (10, 13, 27))
    pct = 100.0 * printable / len(raw)
    sample = bytes(b for b in raw if 32 <= b < 127)[:60].decode("ascii", "replace")
    print("%7d  %5.1f%% printable of %6d bytes | %s" % (rate, pct, len(raw), sample))
    time.sleep(0.5)
