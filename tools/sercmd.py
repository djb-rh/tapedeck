#!/usr/bin/env python3
"""Send one line to the device console and print what comes back, without
toggling DTR/RTS (which would reset the ESP32).
Usage: sercmd.py PORT SECONDS [command ...] [--reset] [--delay=S]"""
import sys, time, serial

delay = 0.0
for a in sys.argv[1:]:
    if a.startswith("--delay="): delay = float(a[8:])
args = [a for a in sys.argv[1:] if a != "--reset" and not a.startswith("--delay=")]
reset = "--reset" in sys.argv
port, secs = args[0], float(args[1])
cmd = " ".join(args[2:])
s = serial.Serial()
s.port = port; s.baudrate = 115200; s.timeout = 0.1
s.rts = False; s.dtr = False
s.open()
if reset:
    s.rts = True; time.sleep(0.1); s.rts = False
s.reset_input_buffer()
t0 = time.time()
while time.time() - t0 < delay:
    d = s.read(4096)
    if d: sys.stdout.write(d.decode("utf-8", "replace")); sys.stdout.flush()
if cmd:
    s.write((cmd + "\n").encode())
t0 = time.time()
while time.time() - t0 < secs:
    d = s.read(4096)
    if d:
        sys.stdout.write(d.decode("utf-8", "replace")); sys.stdout.flush()
s.close()
