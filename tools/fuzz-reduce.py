#!/usr/bin/env python3
"""Reduce a fuzzer input that makes a command fail.

Usage: tools/fuzz-reduce.py [-t SECONDS] [-m TEXT] INPUT OUTPUT COMMAND...

COMMAND is run with the candidate file's path appended (for example
`build/asan/tests/css/fuzz_css replay`). An input "fails" when the command
exits non-zero, is killed by a signal, or runs longer than SECONDS
(default 20; a hang counts as a failure). With -m TEXT the failure must
also print TEXT (to stay on one bug while reducing). The script removes
ever smaller pieces of the input while the command still fails and writes
the smallest failing input to OUTPUT.
"""
import subprocess
import sys


def main():
    args = sys.argv[1:]
    timeout = 20.0
    match = None
    while args and args[0] in ("-t", "-m"):
        if args[0] == "-t":
            timeout = float(args[1])
        else:
            match = args[1].encode()
        args = args[2:]
    if len(args) < 3:
        sys.exit(__doc__)
    src, out, cmd = args[0], args[1], args[2:]
    data = open(src, "rb").read()
    tries = 0

    def fails(candidate):
        nonlocal tries
        tries += 1
        with open(out, "wb") as f:
            f.write(candidate)
        try:
            r = subprocess.run(cmd + [out], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
        except subprocess.TimeoutExpired:
            return match is None
        if r.returncode == 0:
            return False
        return match is None or match in r.stdout

    if not fails(data):
        sys.exit("the input does not fail")
    chunk = max(1, len(data) // 2)
    while chunk >= 1:
        pos = 0
        removed = False
        while pos < len(data):
            candidate = data[:pos] + data[pos + chunk:]
            if fails(candidate):
                data = candidate
                removed = True
            else:
                pos += chunk
        if chunk == 1 and not removed:
            break
        if chunk > 1:
            chunk //= 2
    with open(out, "wb") as f:
        f.write(data)
    print(f"{src}: reduced to {len(data)} bytes in {tries} runs -> {out}")


if __name__ == "__main__":
    main()
