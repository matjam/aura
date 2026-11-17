#!/usr/bin/env python3
"""Serve a page corpus for Aura's page benchmark and save the results.

Usage:
  tools/bench-serve.py [--port 8642] [--root tests/page/samples]
                       [--manifest FILE] [--repeat N] [--settle MS]
                       [--out run/bench/NAME.tsv] [--once]

GET /manifest.txt returns --manifest, or a manifest listing every .html
file at the top of --root. Every other GET serves a file under --root
with Cache-Control: no-store, so each load in every round comes from the
network and the rounds measure the engine, not the HTTP cache. POST
/result writes the body to --out (printing its path); with --once the
server then exits. It listens on 127.0.0.1 only: the QEMU guest reaches
it as 10.0.2.2. See docs/performance.md, "To benchmark pages".
"""
import argparse
import http.server
import os
import sys
import threading


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8642)
    ap.add_argument("--root", default="tests/page/samples")
    ap.add_argument("--manifest")
    ap.add_argument("--repeat", type=int, default=3)
    ap.add_argument("--settle", type=int, default=1000)
    ap.add_argument("--out", default="run/bench/result.tsv")
    ap.add_argument("--once", action="store_true")
    a = ap.parse_args()
    root = os.path.realpath(a.root)

    if a.manifest:
        with open(a.manifest, "rb") as f:
            manifest = f.read()
    else:
        pages = sorted(n for n in os.listdir(root) if n.endswith(".html"))
        lines = ["# every page of %s" % a.root, "repeat %d" % a.repeat, "settle %d" % a.settle]
        manifest = ("\n".join(lines + pages) + "\n").encode()

    done = threading.Event()

    class Handler(http.server.BaseHTTPRequestHandler):
        def send(self, code, body, ctype):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            path = self.path.split("?", 1)[0]
            if path == "/manifest.txt":
                return self.send(200, manifest, "text/plain; charset=utf-8")
            full = os.path.realpath(os.path.join(root, path.lstrip("/")))
            if not full.startswith(root + os.sep) or not os.path.isfile(full):
                return self.send(404, b"not found\n", "text/plain")
            with open(full, "rb") as f:
                body = f.read()
            ext = os.path.splitext(full)[1].lower()
            types = {".html": "text/html; charset=utf-8", ".css": "text/css", ".js": "text/javascript",
                     ".gif": "image/gif", ".png": "image/png", ".jpg": "image/jpeg", ".svg": "image/svg+xml",
                     ".woff": "font/woff", ".woff2": "font/woff2", ".webp": "image/webp"}
            self.send(200, body, types.get(ext, "application/octet-stream"))

        def do_POST(self):
            if self.path.split("?", 1)[0] != "/result":
                return self.send(404, b"not found\n", "text/plain")
            n = int(self.headers.get("Content-Length", "0"))
            if n <= 0 or n > 16 * 1024 * 1024:
                return self.send(400, b"bad length\n", "text/plain")
            body = self.rfile.read(n)
            os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
            with open(a.out, "wb") as f:
                f.write(body)
            print("bench-serve: results in %s" % a.out, flush=True)
            self.send(200, b"saved\n", "text/plain")
            if a.once:
                done.set()

        def log_message(self, fmt, *args):
            pass

    srv = http.server.ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    print("bench-serve: http://127.0.0.1:%d/manifest.txt (guest: http://10.0.2.2:%d/manifest.txt)"
          % (a.port, a.port), flush=True)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    try:
        done.wait()
    except KeyboardInterrupt:
        pass
    srv.shutdown()
    return 0


if __name__ == "__main__":
    sys.exit(main())
