#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run ble_scan reset-only first, then if that works try a full scan."""
import os
import serial, time, sys, re

PORT = os.environ.get('BADGE_PORT', '/dev/cu.wchusbserial10'); BAUD = 115200
s = serial.Serial(PORT, BAUD, timeout=0.2)
log = open('/tmp/bt_scan.log', 'wb')
def out(t):
    sys.stdout.buffer.write(t); sys.stdout.flush(); log.write(t); log.flush()
def slow(line, d=0.005):
    for c in line: s.write(c.encode()); s.flush(); time.sleep(d)
    s.write(b"\n"); s.flush()
def read_for(n):
    deadline = time.time() + n
    while time.time() < deadline:
        c = s.read(2048)
        if c: out(c)
def wait_for(p, t, l):
    dl = time.time() + t; buf = b""; pat = re.compile(p.encode())
    while time.time() < dl:
        c = s.read(2048)
        if c: buf += c; out(c)
        if pat.search(buf): return buf
    sys.stderr.write(f"\nTIMEOUT {l}\n"); return None

s.dtr=False; s.rts=False; time.sleep(0.1); s.rts=True; time.sleep(0.2); s.rts=False
read_for(40); s.reset_input_buffer(); s.write(b"\r"); s.flush(); time.sleep(0.3); read_for(2)
slow("root", 0.08)
if wait_for(r"# $", 30, "prompt") is None: sys.exit(1)

_n=[0]
def run(c, t=15):
    _n[0]+=1; sn=f"S_{_n[0]}_X"
    slow(f"{c}; echo {sn}", 0.003); wait_for(sn+r"\b", t, c[:50])

run("ls -la /sys/class/bluetooth/hci0 2>/dev/null && echo HCI0_OK")
run("/usr/bin/ble_scan 2>&1; echo RC=$?", 60)

# Don't continue to full scan if reset failed.
# Always finish cleanly so the shell doesn't get stuck.
print("\n[scan] done"); log.close(); s.close()
