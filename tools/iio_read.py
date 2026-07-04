#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import os
import serial, time, sys
PORT = os.environ.get('BADGE_PORT', '/dev/cu.wchusbserial10')
s = serial.Serial(PORT, 115200, timeout=0.2)
def out(t): sys.stdout.buffer.write(t); sys.stdout.flush()
def slow_send(line, d=0.005):
    for ch in line:
        s.write(ch.encode()); s.flush(); time.sleep(d)
    s.write(b"\n"); s.flush()
def read_for(t):
    end=time.time()+t
    while time.time()<end:
        c=s.read(2048)
        if c: out(c)

# Already at shell. Just nudge.
s.write(b"\r"); s.flush(); time.sleep(0.3)
read_for(1)

# BME680 readings.
slow_send('echo "--- BME680 ---"'); read_for(1)
slow_send("for f in in_temp_input in_humidityrelative_input in_pressure_input in_resistance_input; do printf '%-28s ' $f; cat /sys/bus/iio/devices/iio:device0/$f 2>/dev/null; done", 0.002); read_for(3)

# BMI270 readings.
slow_send('echo "--- BMI270 ---"'); read_for(1)
slow_send("for f in in_accel_x_raw in_accel_y_raw in_accel_z_raw in_accel_scale in_anglvel_x_raw in_anglvel_y_raw in_anglvel_z_raw in_anglvel_scale; do printf '%-28s ' $f; cat /sys/bus/iio/devices/iio:device1/$f 2>/dev/null; done", 0.002); read_for(3)

print("\n[read done]")
