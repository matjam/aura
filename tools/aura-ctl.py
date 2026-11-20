#!/usr/bin/env python3
"""Drive a running Aura over its control socket (src/browser/control.h).

Usage:
  tools/aura-ctl.py COMMAND...          send one command, print the reply
  tools/aura-ctl.py -                   send each line of stdin as a command
  tools/aura-ctl.py wait [SECONDS]      wait until the page stops loading

Start Aura with the socket, the macOS build for example:
  AURA_CONTROL=/tmp/aura-ctl.sock build/cocoa/src/platform/cocoa/Aura.app/Contents/MacOS/Aura URL &

The socket is AURA_CONTROL, else /tmp/aura-ctl.sock. Exits 1 when the
reply starts with "error". Examples:
  tools/aura-ctl.py eval document.title
  tools/aura-ctl.py eval @                     # the frames
  tools/aura-ctl.py eval "@challenge document.body.innerHTML.slice(0,500)"
  tools/aura-ctl.py clicksel "@checkbox #checkbox"
  tools/aura-ctl.py shot /tmp/aura.png
  tools/aura-ctl.py console 50
"""
import os
import socket
import sys
import time


def send(cmd, path):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(600)
    s.connect(path)
    s.sendall(cmd.encode() + b"\n")
    chunks = []
    while True:
        b = s.recv(65536)
        if not b:
            break
        chunks.append(b)
    s.close()
    return b"".join(chunks).decode(errors="replace").rstrip("\n")


def main():
    path = os.environ.get("AURA_CONTROL", "/tmp/aura-ctl.sock")
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    if args[0] == "wait":
        end = time.time() + (float(args[1]) if len(args) > 1 else 60)
        while time.time() < end:
            if send("busy", path) == "ok no":
                print("ok")
                return
            time.sleep(0.5)
        sys.exit("still loading")
    cmds = [l.rstrip("\n") for l in sys.stdin] if args == ["-"] else [" ".join(args)]
    failed = False
    for c in cmds:
        if not c.strip():
            continue
        r = send(c, path)
        print(r)
        failed |= r.startswith("error")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
