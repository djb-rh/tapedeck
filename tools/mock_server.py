#!/usr/bin/env python3
"""Local stand-in for the TapeDeck HTTP API, for working on web/ without the
device. Serves web/index.html and web/setup.html, keeps jobs in memory and
writes each uploaded job to OUTDIR/job-N.tdj.
Usage: mock_server.py [PORT] [OUTDIR] [--tape MM]"""
import json, os, sys, time, struct
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
args = [a for a in sys.argv[1:] if not a.startswith("--")]
PORT = int(args[0]) if args else 8765
OUT = args[1] if len(args) > 1 else "/tmp"
TAPE = int(sys.argv[sys.argv.index("--tape") + 1]) if "--tape" in sys.argv else 12
GEOM = {4: 24, 6: 32, 9: 50, 12: 70, 18: 112, 24: 128}
jobs = []  # newest first: dict(id, data, meta)
state = {"next": 1, "done": 0, "labels": 0, "last": 0}

def inspect(data, jid, source):
    pages = struct.unpack_from("<H", data, 10)[0]
    copies = struct.unpack_from("<H", data, 8)[0]
    off, cols, h = 64, 0, 0
    for _ in range(pages):
        w, hh = struct.unpack_from("<HH", data, off)
        off += 4 + w * ((hh + 7) // 8); cols += w; h = max(h, hh)
    return {"id": jid, "title": data[16:64].split(b"\0")[0].decode("utf-8", "replace"), "labels": pages,
            "copies": copies, "tape_mm": data[5], "height": h, "length_mm": round(cols * 25.4 / 180, 1),
            "created": struct.unpack_from("<I", data, 12)[0], "source": source, "printed": 0, "bytes": len(data)}

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def send(self, code, body, ctype="application/json"):
        if isinstance(body, (dict, list)): body = json.dumps(body).encode()
        self.send_response(code); self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def do_GET(self):
        p = self.path.split("?")[0]
        if p in ("/", "/index.html"): return self.send(200, open(os.path.join(ROOT, "web/index.html"), "rb").read(), "text/html")
        if p == "/setup": return self.send(200, open(os.path.join(ROOT, "web/setup.html"), "rb").read(), "text/html")
        if p == "/api/status":
            return self.send(200, {"state": "ready", "printer": {"product": "PT-D600", "serial": "MOCK", "error": "", "label_held": False},
                "tape": {"width_mm": TAPE, "print_dots": GEOM[TAPE], "dpi": 180, "media": "Laminated", "color": "White", "ink": "Black",
                         "color_css": "#fbfbf6", "ink_css": "#111111"},
                "job": {"id": 0, "title": "", "done": 0, "total": 0, "queued": 0, "last_id": state["last"], "last_ok": True,
                        "last_error": "", "jobs_done": state["done"], "labels_done": state["labels"]},
                "device": {"version": "mock", "hostname": "tapedeck", "ip": "127.0.0.1", "ap_mode": False, "ssid": "mock", "rssi": -50,
                           "uptime": 1, "heap": 1, "fs_used": 1024, "fs_total": 1500000, "margin_mm": 2.0, "chain": False, "history_max": 25}})
        if p == "/api/jobs": return self.send(200, {"jobs": [j["meta"] for j in jobs]})
        if p.startswith("/api/jobs/"):
            jid = int(p.split("/")[3])
            for j in jobs:
                if j["meta"]["id"] == jid: return self.send(200, j["data"], "application/x-tapedeck-job")
            return self.send(404, {"ok": False, "error": "no such job"})
        if p == "/api/config":
            return self.send(200, {"ssid": "mock", "hostname": "tapedeck", "has_pass": True, "has_web_pass": False, "has_api_key": False,
                                   "tz": "EST5EDT", "ntp": "pool.ntp.org", "margin_mm": 2, "chain": False, "history_max": 25,
                                   "disp_timeout_s": 600, "brightness": 140})
        if p == "/api/events": return self.send(200, ["mock event"])
        if p == "/api/scan": return self.send(200, {"networks": [{"ssid": "Example", "rssi": -48, "open": False}]})
        self.send(404, b"not found", "text/plain")
    def do_POST(self):
        p = self.path.split("?")[0]
        n = int(self.headers.get("Content-Length", 0)); data = self.rfile.read(n)
        if p == "/api/print":
            if data[:4] != b"TDJ1": return self.send(400, {"ok": False, "error": "mock only takes TDJ1"})
            jid = state["next"]; state["next"] += 1
            meta = inspect(data, jid, "web")
            jobs.insert(0, {"meta": meta, "data": data})
            open(os.path.join(OUT, "job-%d.tdj" % jid), "wb").write(data)
            state["done"] += 1; state["labels"] += meta["labels"] * meta["copies"]; state["last"] = jid
            return self.send(200, {"ok": True, "queued": True, "job": meta})
        if p.endswith("/print"): return self.send(200, {"ok": True, "queued": True})
        if p == "/api/config": return self.send(200, {"ok": True})
        self.send(404, b"not found", "text/plain")
    def do_DELETE(self):
        jid = int(self.path.split("/")[3])
        jobs[:] = [j for j in jobs if j["meta"]["id"] != jid]
        self.send(200, {"ok": True})

print("mock TapeDeck on http://127.0.0.1:%d (tape %d mm), jobs -> %s" % (PORT, TAPE, OUT))
ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
