#!/usr/bin/env python3
"""Decode a P-touch raster job (hex dump from the probe's dry-run mode, lines
starting HEX:) and print each page as ASCII art, one text row per head pin.
Usage: decode_job.py capture.txt"""
import sys

def unpackbits(b):
    o, i = bytearray(), 0
    while i < len(b):
        c = b[i] if b[i] < 128 else b[i] - 256
        i += 1
        if c >= 0:
            o += b[i:i + c + 1]; i += c + 1
        elif c != -128:
            o += bytes([b[i]]) * (1 - c); i += 1
    return o

hexs = "".join(l.strip()[4:] for l in open(sys.argv[1]) if l.startswith("HEX:"))
d = bytes.fromhex(hexs)
i, pages, cur, compress = 0, [], [], 0
while i < len(d):
    b = d[i]
    if b == 0 and not cur: i += 1; continue
    if b == 0x1B:
        cmd = d[i + 1]
        if cmd == ord('@'): print("ESC @"); i += 2; continue
        if cmd == ord('i'):
            sub = chr(d[i + 2])
            n = {'a': 1, 'z': 10, 'M': 1, 'K': 1, 'd': 2, 'A': 1, 'S': 0}[sub]
            print("ESC i", sub, d[i + 3:i + 3 + n].hex(" ")); i += 3 + n; continue
        raise SystemExit("unknown ESC %02x at %d" % (cmd, i))
    if b == ord('M'): compress = d[i + 1]; print("M", compress); i += 2; continue
    if b in (ord('G'), ord('g')):
        n = d[i + 1] | d[i + 2] << 8
        data = d[i + 3:i + 3 + n]
        cur.append(unpackbits(data) if compress == 2 and b == ord('G') else data)
        i += 3 + n; continue
    if b == ord('Z'): cur.append(bytes(16)); i += 1; continue
    if b in (0x0C, 0x1A):
        pages.append(cur); cur = []; print("page end %02x, %d lines" % (b, len(pages[-1]))); i += 1; continue
    raise SystemExit("unknown byte %02x at %d" % (b, i))
for n, p in enumerate(pages):
    print("--- page", n + 1)
    rows = []
    for pin in range(128):
        rows.append("".join("#" if (len(l) > pin >> 3 and l[pin >> 3] & (0x80 >> (pin & 7))) else "." for l in p))
    used = [r for r in rows if "#" in r]
    first = next(k for k, r in enumerate(rows) if "#" in r) if used else 0
    last = max(k for k, r in enumerate(rows) if "#" in r) if used else 0
    for k in range(first, last + 1):
        print("%3d %s" % (k, rows[k]))
