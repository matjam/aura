#!/usr/bin/env python3
"""Generate Domato test pages with a fixed seed.

Usage: python3 -I tests/domato/generate.py DOMATO_DIR OUT_DIR COUNT SEED

Domato's generator draws from Python's `random` module and has no seed
option; this seeds the module and then runs generator.py as Domato's
README says (--output_dir, --no_of_files), so the same seed gives the
same pages. Run it with -I: the Domato folder is put on the module path
here and nothing else is.
"""
import os
import random
import runpy
import sys


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    domato, out, count, seed = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
    domato = os.path.abspath(domato)
    out = os.path.abspath(out)
    os.makedirs(out, exist_ok=True)
    random.seed(seed)
    sys.path.insert(0, domato)
    os.chdir(domato)
    sys.argv = [os.path.join(domato, "generator.py"), "--output_dir", out, "--no_of_files", str(count)]
    runpy.run_path(sys.argv[0], run_name="__main__")


if __name__ == "__main__":
    main()
