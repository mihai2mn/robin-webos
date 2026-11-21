#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Tiny static server for the TV benchmark pages (runs ON the TV, 127.0.0.1 only). Supports Range. The pages report to
# /result?ua=<query>&r=<text> and /log?<text>; both are appended to results.log next to this file.
import http.server, os, re, sys, urllib.parse
ROOT = os.path.dirname(os.path.abspath(__file__))
LOG = os.path.join(ROOT, "results.log")
GIF = b"GIF89a\x01\x00\x01\x00\x80\x00\x00\x00\x00\x00\xff\xff\xff!\xf9\x04\x01\x00\x00\x00\x00,\x00\x00\x00\x00\x01\x00\x01\x00\x00\x02\x02D\x01\x00;"
class H(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        u = urllib.parse.urlsplit(self.path)
        if u.path in ("/result", "/log"):
            q = urllib.parse.parse_qs(u.query)
            line = (q.get("ua", [""])[0] + "\t" + q.get("r", [""])[0]) if u.path == "/result" else urllib.parse.unquote(u.query)
            with open(LOG, "a", encoding="utf-8") as f: f.write(line + "\n")
            self.send_response(200); self.send_header("Content-Type", "image/gif"); self.send_header("Cache-Control", "no-store"); self.end_headers(); self.wfile.write(GIF); return
        p = os.path.join(ROOT, os.path.normpath(u.path.lstrip("/") or "index.html"))
        if not os.path.isfile(p): self.send_error(404); return
        size = os.path.getsize(p); start, end, st = 0, size - 1, 200
        m = re.match(r"bytes=(\d*)-(\d*)", self.headers.get("Range") or "")
        if m:
            if m.group(1): start = int(m.group(1))
            if m.group(2): end = min(int(m.group(2)), size - 1)
            st = 206
        self.send_response(st); self.send_header("Content-Type", self.guess_type(p)); self.send_header("Accept-Ranges", "bytes"); self.send_header("Cache-Control", "no-store")
        if st == 206: self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, size))
        self.send_header("Content-Length", str(end - start + 1)); self.end_headers()
        with open(p, "rb") as f:
            f.seek(start); rem = end - start + 1
            while rem > 0:
                c = f.read(min(65536, rem))
                if not c: break
                self.wfile.write(c); rem -= len(c)
if __name__ == "__main__":
    http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1]) if len(sys.argv) > 1 else 18899), H).serve_forever()
