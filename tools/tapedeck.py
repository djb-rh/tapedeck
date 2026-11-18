#!/usr/bin/env python3
"""Command-line client for a TapeDeck label printer. Standard library only.

  tapedeck.py status
  tapedeck.py text "136078-1015|27C512 U5" "136078-1016|27C512 U6"   # one label per argument, | = new line
  tapedeck.py text --font mono --frame --copies 3 "KEEP OUT"
  tapedeck.py image logo.png [--threshold 50] [--invert]               # macOS: scaled to the tape with sips
  tapedeck.py pbm logo.pbm [--title logo]
  tapedeck.py jobs | reprint ID | delete ID

Host: --host or $TAPEDECK_HOST (default tapedeck.local). API key: --key or
$TAPEDECK_KEY. --save stores the job in the history without printing.
"""
import argparse, json, os, struct, subprocess, sys, tempfile, urllib.error, urllib.request

def req(args, method, path, body=None, headers=None):
    url = "http://%s%s" % (args.host, path)
    h = dict(headers or {})
    if args.key: h["X-API-Key"] = args.key
    r = urllib.request.Request(url, data=body, method=method, headers=h)
    try:
        with urllib.request.urlopen(r, timeout=30) as resp:
            data = resp.read()
    except urllib.error.HTTPError as e:
        data = e.read()
    except urllib.error.URLError as e:
        sys.exit("cannot reach %s: %s" % (args.host, e.reason))
    try:
        return json.loads(data)
    except ValueError:
        return data

def report(j):
    if isinstance(j, dict) and "job" in j:
        job = j["job"]
        state = "queued" if j.get("queued") else ("saved" if j.get("ok") else "failed")
        print("job #%s %s: %d label(s) x%d, %.1f mm, %s" % (job.get("id"), state, job.get("labels", 0), job.get("copies", 1),
                                                         job.get("length_mm", 0), job.get("title", "")))
    if isinstance(j, dict) and not j.get("ok", True):
        sys.exit("error: " + j.get("error", "?"))

def cmd_status(args):
    s = req(args, "GET", "/api/status")
    t = s.get("tape") or {}
    print("state:   %s%s" % (s["state"], " (%s)" % s["printer"]["error"] if s["printer"].get("error") else ""))
    print("printer: %s %s" % (s["printer"].get("product") or "-", s["printer"].get("serial") or ""))
    if t:
        print("tape:    %s mm %s, %s on %s (%d printable dots)" % (t["width_mm"], t["media"], t["ink"], t["color"], t["print_dots"]))
    j = s["job"]
    if j["id"]: print("job:     #%d %s, %d of %d" % (j["id"], j["title"], j["done"], j["total"]))
    if j["last_error"]: print("last:    #%d failed: %s" % (j["last_id"], j["last_error"]))
    d = s["device"]
    print("device:  %s.local %s, v%s" % (d["hostname"], d["ip"], d["version"]))

def cmd_text(args):
    labels = []
    for t in args.labels:
        lab = {"text": t.replace("|", "\n")}
        labels.append(lab)
    body = {"labels": labels, "copies": args.copies, "font": args.font, "align": args.align}
    if args.frame: body["frame"] = True
    if args.invert: body["invert"] = True
    if args.length: body["length_mm"] = args.length
    if args.size: body["size"] = args.size
    if args.title: body["title"] = args.title
    if args.chain: body["chain"] = True
    path = "/api/save" if args.save else "/api/print"
    report(req(args, "POST", path, json.dumps(body).encode(), {"Content-Type": "application/json"}))

def tape_dots(args):
    s = req(args, "GET", "/api/status")
    if not s.get("tape") or not s["tape"].get("print_dots"):
        sys.exit("printer reports no tape; cannot size the image")
    return s["tape"]["print_dots"]

def read_bmp(path):
    d = open(path, "rb").read()
    if d[:2] != b"BM": raise ValueError("not a BMP")
    off, = struct.unpack_from("<I", d, 10)
    w, h, _, bpp = struct.unpack_from("<iiHH", d, 18)
    flip = h > 0; h = abs(h)
    stride = ((w * bpp + 31) // 32) * 4
    px = []
    for y in range(h):
        row = d[off + (h - 1 - y if flip else y) * stride:]
        line = []
        for x in range(w):
            if bpp in (24, 32):
                b, g, r = row[x * bpp // 8], row[x * bpp // 8 + 1], row[x * bpp // 8 + 2]
                a = row[x * 4 + 3] if bpp == 32 else 255
                lum = (r * .299 + g * .587 + b * .114) * a / 255 + 255 * (1 - a / 255)
            else:
                raise ValueError("unsupported BMP depth %d" % bpp)
            line.append(lum)
        px.append(line)
    return w, h, px

def cmd_image(args):
    dots = args.height or tape_dots(args)
    with tempfile.TemporaryDirectory() as td:
        bmp = os.path.join(td, "img.bmp")
        try:
            subprocess.run(["sips", "--resampleHeight", str(dots), "-s", "format", "bmp", args.file, "--out", bmp],
                           check=True, capture_output=True)
        except (OSError, subprocess.CalledProcessError) as e:
            sys.exit("could not convert the image with sips (macOS only); make a PBM instead: %s" % e)
        w, h, px = read_bmp(bmp)
    t = 255 * args.threshold / 100
    stride = (w + 7) // 8
    bits = bytearray(stride * h)
    for y in range(h):
        for x in range(w):
            on = px[y][x] < t
            if args.invert: on = not on
            if on: bits[y * stride + (x >> 3)] |= 0x80 >> (x & 7)
    pbm = b"P4\n%d %d\n" % (w, h) + bytes(bits)
    send_pbm(args, pbm, args.title or os.path.basename(args.file))

def send_pbm(args, pbm, title):
    path = "/api/save" if args.save else "/api/print"
    report(req(args, "POST", path, pbm, {"Content-Type": "image/x-portable-bitmap", "X-Title": title[:47], "X-Copies": str(args.copies)}))

def cmd_pbm(args):
    send_pbm(args, open(args.file, "rb").read(), args.title or os.path.basename(args.file))

def cmd_jobs(args):
    for j in req(args, "GET", "/api/jobs")["jobs"]:
        print("#%-4d %2d label(s) x%-2d %6.1f mm  %-5s %s" % (j["id"], j["labels"], j["copies"], j["length_mm"], j["source"], j["title"]))

def cmd_reprint(args):
    j = req(args, "POST", "/api/jobs/%d/print" % args.id)
    print("queued" if j.get("ok") else "error: " + j.get("error", "?"))

def cmd_delete(args):
    j = req(args, "DELETE", "/api/jobs/%d" % args.id)
    print("deleted" if j.get("ok") else "error: " + j.get("error", "?"))

def main():
    p = argparse.ArgumentParser(description="TapeDeck label printer client")
    p.add_argument("--host", default=os.environ.get("TAPEDECK_HOST", "tapedeck.local"))
    p.add_argument("--key", default=os.environ.get("TAPEDECK_KEY", ""))
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("status").set_defaults(fn=cmd_status)
    t = sub.add_parser("text", help="print text labels (one per argument, | for a new line)")
    t.add_argument("labels", nargs="+")
    t.add_argument("--font", default="bold", choices=["sans", "bold", "mono", "serif"])
    t.add_argument("--align", default="center", choices=["left", "center", "right"])
    t.add_argument("--size", type=int, help="line height in dots (default: fit the tape)")
    t.add_argument("--length", type=float, help="fixed label length in mm")
    t.add_argument("--frame", action="store_true")
    t.add_argument("--invert", action="store_true")
    t.add_argument("--chain", action="store_true", help="hold the last label in the printer")
    t.add_argument("--copies", type=int, default=1, help="copies of the whole set")
    t.add_argument("--title")
    t.add_argument("--save", action="store_true", help="store in the history, don't print")
    t.set_defaults(fn=cmd_text)
    i = sub.add_parser("image", help="print an image scaled to the tape (macOS sips)")
    i.add_argument("file")
    i.add_argument("--threshold", type=float, default=50)
    i.add_argument("--invert", action="store_true")
    i.add_argument("--height", type=int, help="dots across the tape (default: the loaded tape)")
    i.add_argument("--copies", type=int, default=1)
    i.add_argument("--title")
    i.add_argument("--save", action="store_true")
    i.set_defaults(fn=cmd_image)
    b = sub.add_parser("pbm", help="print a binary PBM (P4) exactly as given")
    b.add_argument("file")
    b.add_argument("--copies", type=int, default=1)
    b.add_argument("--title")
    b.add_argument("--save", action="store_true")
    b.set_defaults(fn=cmd_pbm)
    sub.add_parser("jobs").set_defaults(fn=cmd_jobs)
    r = sub.add_parser("reprint"); r.add_argument("id", type=int); r.set_defaults(fn=cmd_reprint)
    d = sub.add_parser("delete"); d.add_argument("id", type=int); d.set_defaults(fn=cmd_delete)
    args = p.parse_args()
    args.fn(args)

if __name__ == "__main__":
    main()
