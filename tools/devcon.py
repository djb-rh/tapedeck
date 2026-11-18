#!/usr/bin/env python3
"""Talk to the TapeDeck serial console.
  devcon.py PORT [--boot] CMD [CMD ...]        run commands, print replies
  devcon.py PORT [--boot] --shot out.png        screen dump to PNG
  devcon.py PORT [--boot] --job ID out.png      job file -> PNG preview (decoded TDJ1)
  devcon.py PORT [--boot] --put file.tdj        upload a job file into the history
Opening the port on this Mac resets the Core; --boot waits for it to come up."""
import sys, time, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import serial
from pngout import write_png

def open_port(port, boot):
    s = serial.Serial(); s.port = port; s.baudrate = 115200; s.timeout = 0.1
    s.rts = False; s.dtr = False; s.open()
    if boot:
        t = time.time()
        while time.time() - t < 9:
            d = s.read(4096)
            if d: sys.stderr.write(d.decode("utf-8", "replace"))
    s.reset_input_buffer()
    return s

def run(s, cmd, until=None, secs=4.0):
    s.write((cmd + "\n").encode())
    out, t = b"", time.time()
    while time.time() - t < secs:
        d = s.read(65536)
        if d:
            out += d
            if until and until.encode() in out: break
    return out.decode("utf-8", "replace")

def tdj_to_png(data, path, tape=(250, 250, 244), ink=(17, 17, 17), scale=3):
    assert data[:4] == b"TDJ1", "not a TDJ1 file"
    pages = struct.unpack_from("<H", data, 10)[0]
    off, imgs = 64, []
    for _ in range(pages):
        w, h = struct.unpack_from("<HH", data, off); off += 4
        cb = (h + 7) // 8
        cols = [data[off + x * cb: off + (x + 1) * cb] for x in range(w)]
        off += w * cb
        imgs.append((w, h, cols))
    gap = 8
    H = max(i[1] for i in imgs)
    Wt = sum(i[0] for i in imgs) + gap * (len(imgs) - 1)
    rows = []
    for y in range(H):
        row = bytearray()
        for n, (w, h, cols) in enumerate(imgs):
            yy = y - (H - h) // 2
            for x in range(w):
                on = 0 <= yy < h and cols[x][yy >> 3] & (0x80 >> (yy & 7))
                row += bytes(ink if on else tape)
            if n + 1 < len(imgs): row += bytes((60, 60, 60)) * gap
        rows.append(row)
    # integer upscale
    big = []
    for r in rows:
        br = bytearray()
        for i in range(0, len(r), 3): br += r[i:i + 3] * scale
        big += [br] * scale
    write_png(path, Wt * scale, H * scale, big)
    print("wrote %s: %d pages, %dx%d dots" % (path, pages, Wt, H))

def main():
    a = sys.argv[1:]
    port = a.pop(0)
    boot = "--boot" in a
    a = [x for x in a if x != "--boot"]
    s = open_port(port, boot)
    if a and a[0] == "--shot":
        txt = run(s, "shot", "END", 20)
        lines = txt.splitlines()
        i = next(k for k, l in enumerate(lines) if l.startswith("SHOT"))
        w, h = map(int, lines[i].split()[1:3])
        rows = []
        for l in lines[i + 1:]:
            if not l.startswith("R"): break
            row = bytearray()
            for tok in l.split()[1:]:
                k, v = tok.split(":"); k = int(k, 16); v = int(v, 16)
                v = ((v & 0xFF) << 8) | (v >> 8)  # LGFX readRect returns byte-swapped RGB565
                r = (v >> 11) & 31; g = (v >> 5) & 63; b = v & 31
                row += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))) * k
            rows.append(row)
        write_png(a[1], w, len(rows), rows)
        print("wrote", a[1])
    elif a and a[0] == "--put":
        data = open(a[1], "rb").read()
        run(s, "putbegin", "ok", 3)
        for i in range(0, len(data), 900):
            r = run(s, "put " + data[i:i + 900].hex(), "ok", 5)
            if "ok" not in r: raise SystemExit("put failed: " + r)
        print(run(s, "putend", "job", 5).strip())
    elif a and a[0] == "--job":
        txt = run(s, "dump " + a[1], "END", 20)
        hexs = "".join(l.strip()[4:] for l in txt.splitlines() if l.startswith("HEX:"))
        tdj_to_png(bytes.fromhex(hexs), a[2])
    else:
        for c in a:
            print(run(s, c, None, 3 if not c.startswith(("text", "render")) else 6))

if __name__ == "__main__":
    main()

def shot_to_png(s, path):
    txt = run(s, "shot", "END", 25)
    lines = txt.splitlines()
    i = next(k for k, l in enumerate(lines) if l.startswith("SHOT"))
    w, h = map(int, lines[i].split()[1:3])
    rows = []
    for l in lines[i + 1:]:
        if not l.startswith("R"): break
        row = bytearray()
        for tok in l.split()[1:]:
            k, v = tok.split(":"); k = int(k, 16); v = int(v, 16)
            v = ((v & 0xFF) << 8) | (v >> 8)
            r = (v >> 11) & 31; g = (v >> 5) & 63; b = v & 31
            row += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))) * k
        rows.append(row)
    write_png(path, w, len(rows), rows)
