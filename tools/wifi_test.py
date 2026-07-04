#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Reset → boot → log in (slowly) → wifi-connect → ping.

Slow-typed login: each byte sent with a delay so we don't race kernel
printk that's still streaming to ttyS0 around the login moment.
"""
import os
import serial, time, sys, re

PORT = os.environ.get('BADGE_PORT', '/dev/cu.wchusbserial10')
BAUD = 115200
SSID = "Radio Grandma"
PSK  = "StormMakerWaspAhead4Tart"

s = serial.Serial(PORT, BAUD, timeout=0.2)

def out(text):
    sys.stdout.write(text); sys.stdout.flush()

def slow_send(line, char_delay=0.05):
    for ch in line:
        s.write(ch.encode())
        s.flush()
        time.sleep(char_delay)
    s.write(b"\n"); s.flush()

def read_for(seconds, accumulate=True):
    deadline = time.time() + seconds
    buf = ""
    while time.time() < deadline:
        chunk = s.read(2048)
        if chunk:
            txt = chunk.decode(errors='replace')
            buf += txt if accumulate else ""
            out(txt)
    return buf

def wait_for(pattern_str, timeout, label):
    deadline = time.time() + timeout
    buf = ""
    pat = re.compile(pattern_str)
    while time.time() < deadline:
        chunk = s.read(2048)
        if chunk:
            txt = chunk.decode(errors='replace')
            buf += txt
            out(txt)
            if pat.search(buf):
                return buf
    sys.stderr.write(f"\n[wifi_test] TIMEOUT waiting for {label} ({pattern_str!r}) after {timeout}s\n")
    return None

# Hardware reset.
s.dtr = False; s.rts = False; time.sleep(0.1)
s.rts = True;  time.sleep(0.2); s.rts = False

# Wait for FULL boot to complete and quiesce — drain past the 6.7s "blit" line.
read_for(15)

# Drain any in-flight bytes, then nudge with a CR to force a fresh prompt.
s.reset_input_buffer()
s.write(b"\r"); s.flush()
time.sleep(0.3)
read_for(1)

# Slowly type "root" so kernel printk can't slice across our bytes.
slow_send("root", char_delay=0.08)

# Now wait for the actual shell prompt "# ".
if wait_for(r"# $", 10, "shell # prompt") is None:
    sys.stderr.write("[wifi_test] never reached shell prompt\n")
    sys.exit(1)

# Sanity sentinel — confirm the shell is responding.
sentinel = f"OK{int(time.time())%1000}"
slow_send(f"echo {sentinel}", char_delay=0.02)
if wait_for(re.escape(sentinel) + r"\b", 5, "sentinel echo") is None:
    sys.exit(1)

# Run wifi-connect.
slow_send(f'wifi-connect "{SSID}" {PSK}', char_delay=0.01)
# wifi-connect: ip(1s) + wpa(2s) + sleep(5s) + udhcpc(up to 30s) = ~40s typical
read_for(60)

# Address + ping.
slow_send('ip -4 addr show wlan0', char_delay=0.01)
read_for(3)
# basic busybox ping (no FANCY_PING) doesn't accept -c; just spam a
# few seconds and Ctrl+C.
slow_send('ping 1.1.1.1', char_delay=0.01)
read_for(8)
s.write(b'\x03')  # Ctrl+C
read_for(3)

s.close()
sys.stderr.write("\n[wifi_test] done.\n")
