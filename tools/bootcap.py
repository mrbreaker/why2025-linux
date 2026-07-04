#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Reset the ESP32-P4 (side USB-C / CH340) and capture serial output during boot.

Usage:
    python3 tools/bootcap.py [duration_seconds] > /tmp/bootcap.log

Why this exists:
- esptool's --after hard-reset releases the port, then the kernel boot output
  streams to the next opener.  By the time you can re-open the port, you've
  already missed the early boot logs.
- This script holds the serial port open the whole time, pulses RTS to reset
  the P4, then reads continuously, so the entire boot stream is captured.

Defaults to /dev/cu.wchusbserial10 — adjust PORT below for your setup.
The tio program does not have an "exit after N seconds" mode, which is why
this exists.

After capture, grep the log for what you care about, e.g.:
    grep -E "esp32_sdio|cmd_init|wlan|read_packet" /tmp/bootcap.log
"""
import os
import serial, time, sys

PORT = os.environ.get('BADGE_PORT', '/dev/cu.wchusbserial10')
BAUD = 115200
DUR  = float(sys.argv[1]) if len(sys.argv) > 1 else 14.0

s = serial.Serial(PORT, BAUD, timeout=0.3)
# Manual hard reset: ensure both lines are low first, then pulse RTS high-low
s.dtr = False
s.rts = False
time.sleep(0.1)
s.rts = True   # assert reset (CH340 RTS drives EN via inverter on this badge)
time.sleep(0.2)
s.rts = False  # release reset

end = time.time() + DUR
out = bytearray()
while time.time() < end:
    chunk = s.read(8192)
    if chunk:
        out.extend(chunk)
s.close()
sys.stdout.buffer.write(bytes(out))
sys.stdout.flush()
sys.stderr.write(f'\n--- captured {len(out)} bytes in {DUR}s ---\n')
