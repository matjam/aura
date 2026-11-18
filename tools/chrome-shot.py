#!/usr/bin/env python3
"""Screenshot a page with headless Chrome at an exact viewport size.

Usage:
  tools/chrome-shot.py [--chrome PATH] [--wait MS] [--timeout S]
                       [--eval EXPR]... URL WIDTH HEIGHT OUT.png

Headless Chrome's --window-size gives a viewport of at least 500 pixels
wide and 87 pixels shorter than asked (Chrome 142 on macOS, 2026-11-17),
so a 400x900 --screenshot is a 500x813 layout cut to 400x900. This tool
drives Chrome through the DevTools protocol instead: it sets the
viewport with Emulation.setDeviceMetricsOverride, loads URL, waits for
the load event and then MS more milliseconds (default 2000), prints the
value of each --eval expression, and writes the screenshot.

Light colour scheme, no scroll bars, a fresh profile in a temporary
directory. Pure Python (the standard library only).
"""

import base64
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

MAC_CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"


class Socket:
    """A WebSocket client: text frames only, enough for the protocol."""

    def __init__(self, url):
        assert url.startswith("ws://")
        host_port, path = url[5:].split("/", 1)
        host, port = host_port.split(":")
        self.s = socket.create_connection((host, int(port)), timeout=30)
        key = base64.b64encode(os.urandom(16)).decode()
        req = (
            f"GET /{path} HTTP/1.1\r\nHost: {host_port}\r\nUpgrade: websocket\r\n"
            f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n"
        )
        self.s.sendall(req.encode())
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = self.s.recv(1)
            if not chunk:
                raise SystemExit("chrome-shot: the DevTools socket closed during the handshake")
            head += chunk
        if b" 101 " not in head.split(b"\r\n", 1)[0]:
            raise SystemExit("chrome-shot: the DevTools socket refused the upgrade")
        self.buf = b""

    def send(self, text):
        data = text.encode()
        n = len(data)
        if n < 126:
            hdr = bytes([0x81, 0x80 | n])
        elif n < 65536:
            hdr = bytes([0x81, 0x80 | 126]) + n.to_bytes(2, "big")
        else:
            hdr = bytes([0x81, 0x80 | 127]) + n.to_bytes(8, "big")
        mask = os.urandom(4)
        body = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
        self.s.sendall(hdr + mask + body)

    def _read(self, n):
        while len(self.buf) < n:
            chunk = self.s.recv(65536)
            if not chunk:
                raise SystemExit("chrome-shot: the DevTools socket closed")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def recv(self):
        message = b""
        while True:
            b0, b1 = self._read(2)
            n = b1 & 0x7F
            if n == 126:
                n = int.from_bytes(self._read(2), "big")
            elif n == 127:
                n = int.from_bytes(self._read(8), "big")
            mask = self._read(4) if b1 & 0x80 else None
            data = self._read(n)
            if mask:
                data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
            op = b0 & 0x0F
            if op == 8:
                raise SystemExit("chrome-shot: the DevTools socket closed")
            if op in (9, 10):
                continue
            message += data
            if b0 & 0x80:
                return message.decode()


class Page:
    def __init__(self, ws):
        self.ws = ws
        self.next_id = 0
        self.events = []

    def call(self, method, **params):
        self.next_id += 1
        my_id = self.next_id
        self.ws.send(json.dumps({"id": my_id, "method": method, "params": params}))
        while True:
            msg = json.loads(self.ws.recv())
            if msg.get("id") == my_id:
                if "error" in msg:
                    raise SystemExit(f"chrome-shot: {method}: {msg['error'].get('message')}")
                return msg.get("result", {})
            if "method" in msg:
                self.events.append(msg["method"])

    def wait_event(self, name, seconds):
        if name in self.events:
            return True
        self.ws.s.settimeout(seconds)
        end = time.time() + seconds
        try:
            while time.time() < end:
                msg = json.loads(self.ws.recv())
                if msg.get("method") == name:
                    return True
        except (socket.timeout, TimeoutError):
            return False
        finally:
            self.ws.s.settimeout(30)
        return False


def main(argv):
    chrome = os.environ.get("CHROME") or (MAC_CHROME if os.path.exists(MAC_CHROME) else "google-chrome-stable")
    wait_ms, limit, evals = 2000, 90, []
    args = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--chrome" and i + 1 < len(argv):
            chrome = argv[i + 1]
            i += 2
        elif a == "--wait" and i + 1 < len(argv):
            wait_ms = int(argv[i + 1])
            i += 2
        elif a == "--timeout" and i + 1 < len(argv):
            limit = int(argv[i + 1])
            i += 2
        elif a == "--eval" and i + 1 < len(argv):
            evals.append(argv[i + 1])
            i += 2
        else:
            args.append(a)
            i += 1
    if len(args) != 4:
        sys.stderr.write(__doc__)
        return 2
    url, width, height, out = args[0], int(args[1]), int(args[2]), args[3]
    profile = tempfile.mkdtemp(prefix="chrome-shot.")
    proc = subprocess.Popen(
        [chrome, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--no-first-run",
         "--no-default-browser-check", "--remote-debugging-port=0", f"--user-data-dir={profile}",
         "--window-size=1280,1000", "about:blank"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        port_file = os.path.join(profile, "DevToolsActivePort")
        for _ in range(200):
            if os.path.exists(port_file) and os.path.getsize(port_file) > 0:
                break
            time.sleep(0.05)
        else:
            raise SystemExit("chrome-shot: Chrome did not open its DevTools port")
        port = open(port_file).read().split()[0]
        targets = json.load(urllib.request.urlopen(f"http://127.0.0.1:{port}/json/list"))
        page_target = next(t for t in targets if t.get("type") == "page")
        page = Page(Socket(page_target["webSocketDebuggerUrl"]))
        page.call("Page.enable")
        page.call("Emulation.setDeviceMetricsOverride", width=width, height=height, deviceScaleFactor=1,
                  mobile=False)
        page.call("Emulation.setEmulatedMedia", features=[{"name": "prefers-color-scheme", "value": "light"}])
        page.events.clear()
        page.call("Page.navigate", url=url)
        if not page.wait_event("Page.loadEventFired", limit):
            print("chrome-shot: no load event; screenshot taken anyway", file=sys.stderr)
        time.sleep(wait_ms / 1000)
        for e in evals:
            r = page.call("Runtime.evaluate", expression=e, returnByValue=True)
            v = r.get("result", {}).get("value", r.get("result", {}).get("description"))
            print(f"eval {e}: {v}")
        shot = page.call("Page.captureScreenshot", format="png")
        with open(out, "wb") as f:
            f.write(base64.b64decode(shot["data"]))
    finally:
        proc.terminate()
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()
        shutil.rmtree(profile, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
