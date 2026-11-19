#!/usr/bin/env python3
"""Sample where Aura spends its time in the guest, by function.

Usage:
  tools/guest-profile.py sample SECONDS OUT   read the CPU's NIP and LR
                                              about 140 times a second
  tools/guest-profile.py report SAMPLES [TOP] the functions with the most
                                              samples, and who called them

`sample` asks QEMU's monitor for the registers ("info registers"); the
guest keeps running. `report` maps each address to a symbol of
build/ppc/src/platform/macos9/Aura.xcoff (set AURA_XCOFF for another
build). It finds where Mac OS loaded Aura's code by reading the guest's
memory at the commonest addresses and finding those bytes in the
XCOFF's .text, so Aura must still be running, the same build, when it
reports.

QEMU updates NIP only at the end of a translated block, so samples
inside one function land on the first instruction of its blocks: they
count how often each block ran times its cost, not single instructions.
In the interpreter a block is one opcode's handler. Samples outside
Aura are Mac OS: the idle loop (one address) or the Toolbox.

The socket is QMP_SOCK, else AURA_RUN/qmp.sock, else run/qmp.sock.
"""
import bisect
import collections
import importlib.util
import os
import re
import struct
import subprocess
import sys
import time

here = os.path.dirname(os.path.abspath(__file__))
repo = os.path.dirname(here)
spec = importlib.util.spec_from_file_location("guest_qmp", os.path.join(here, "guest-qmp.py"))
gq = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gq)


def sample(seconds, out_path):
    q = gq.QMP()
    nip_re = re.compile(r"NIP\s+([0-9a-fA-F]+)")
    lr_re = re.compile(r"LR\s+([0-9a-fA-F]+)")
    end = time.time() + seconds
    n = 0
    with open(out_path, "w") as out:
        while time.time() < end:
            regs = q.hmp("info registers")
            nip = nip_re.search(regs)
            lr = lr_re.search(regs)
            if nip:
                out.write(f"{nip.group(1)} {lr.group(1) if lr else '0'}\n")
                n += 1
            time.sleep(0.005)
    print(f"{n} samples in {out_path}")


def tool(name):
    prefix = os.environ.get("RETRO68")
    if not prefix:
        prefix = subprocess.run(["sh", "-c", f". {repo}/toolchain/find.sh && echo $RETRO68"],
                                capture_output=True, text=True).stdout.strip()
    return os.path.join(prefix, "bin", "powerpc-apple-macos-" + name)


def text_section(xcoff):
    """The .text bytes and their file offset, from objdump -h."""
    out = subprocess.run([tool("objdump"), "-h", xcoff], capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        f = line.split()
        if len(f) >= 6 and f[1] == ".text":
            size, off = int(f[2], 16), int(f[5], 16)
            with open(xcoff, "rb") as fh:
                fh.seek(off)
                return fh.read(size)
    sys.exit("no .text in " + xcoff)


def symbols(xcoff):
    out = subprocess.run([tool("nm"), "-n", xcoff], capture_output=True, text=True, check=True).stdout
    syms = []
    for line in out.splitlines():
        f = line.split()
        if len(f) == 3 and f[1] in "tT":
            syms.append((int(f[0], 16), f[2].lstrip(".")))
    syms.sort()
    return syms


def find_base(samples, text):
    q = gq.QMP()
    for nip, _ in collections.Counter(s[0] for s in samples).most_common(40):
        at = (nip & ~3) - 32
        words = q.read_words(at, 24, "w")
        if not words:
            continue
        blob = b"".join(struct.pack(">I", w) for w in words)
        i = text.find(blob)
        if i >= 0 and text.find(blob, i + 1) < 0:
            return at - i
    sys.exit("could not find Aura's code in the guest: is the same build running?")


def report(path, top):
    samples = [tuple(int(x, 16) for x in line.split()) for line in open(path) if line.strip()]
    if not samples:
        sys.exit("no samples")
    xcoff = os.environ.get("AURA_XCOFF", os.path.join(repo, "build/ppc/src/platform/macos9/Aura.xcoff"))
    text = text_section(xcoff)
    syms = symbols(xcoff)
    addrs = [a for a, _ in syms]
    base = find_base(samples, text)

    def name(pc):
        off = pc - base
        if not 0 <= off < len(text):
            return None
        i = bisect.bisect_right(addrs, off) - 1
        return syms[i][1] if i >= 0 else "?"

    n = len(samples)
    own = collections.Counter()
    pairs = collections.Counter()
    outside = collections.Counter()
    for nip, lr in samples:
        s = name(nip)
        if s is None:
            outside[nip] += 1
            s = "(Mac OS)"
        own[s] += 1
        pairs[(s, name(lr) or "(Mac OS)")] += 1
    print(f"{n} samples; Aura's code at {base:#x}")
    print(f"\n{'samples':>7} {'share':>6}  function")
    for s, c in own.most_common(top):
        print(f"{c:7d} {100 * c / n:5.1f}%  {s}")
    print(f"\n{'samples':>7} {'share':>6}  function <- caller (LR)")
    for (s, l), c in pairs.most_common(top):
        print(f"{c:7d} {100 * c / n:5.1f}%  {s} <- {l}")
    print(f"\n{'samples':>7} {'share':>6}  Mac OS address (one address with many samples is the idle loop)")
    for a, c in outside.most_common(5):
        print(f"{c:7d} {100 * c / n:5.1f}%  {a:#010x}")


def main(argv):
    if len(argv) >= 3 and argv[0] == "sample":
        sample(float(argv[1]), argv[2])
    elif len(argv) >= 2 and argv[0] == "report":
        report(argv[1], int(argv[2]) if len(argv) > 2 else 40)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
