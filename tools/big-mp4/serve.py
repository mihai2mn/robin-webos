#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A static file server with HTTP Range support (Python's http.server has none), for media tests.

    serve.py DIR [PORT]     logs each request's Range header to stderr
"""
import os
import re
import sys
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer


class Handler(SimpleHTTPRequestHandler):
    def send_head(self):
        path = self.translate_path(self.path)
        if os.path.isdir(path) or not os.path.exists(path):
            return super().send_head()
        size = os.path.getsize(path)
        rng = self.headers.get("Range")
        sys.stderr.write(f"GET {self.path} Range={rng} size={size}\n")
        f = open(path, "rb")
        m = re.match(r"bytes=(\d+)-(\d*)$", rng or "")
        if not m:
            self.send_response(200)
            self.send_header("Content-Length", str(size))
            start, end = 0, size - 1
        else:
            start = int(m.group(1))
            end = int(m.group(2)) if m.group(2) else size - 1
            if start >= size:
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{size}")
                self.end_headers()
                f.close()
                return None
            end = min(end, size - 1)
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
            self.send_header("Content-Length", str(end - start + 1))
        self.send_header("Content-Type", self.guess_type(path))
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()
        f.seek(start)
        self._remaining = end - start + 1
        return f

    def copyfile(self, source, outputfile):
        remaining = getattr(self, "_remaining", None)
        while remaining is None or remaining > 0:
            chunk = source.read(min(1 << 20, remaining) if remaining else 1 << 20)
            if not chunk:
                break
            try:
                outputfile.write(chunk)
            except (BrokenPipeError, ConnectionResetError):
                break
            if remaining is not None:
                remaining -= len(chunk)


if __name__ == "__main__":
    os.chdir(sys.argv[1])
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8099
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
