#!/usr/bin/env python3
"""Summarize or compare page benchmark results (src/browser/bench.h).

Usage:
  tools/bench-compare.py RUN.tsv            medians per page
  tools/bench-compare.py OLD.tsv NEW.tsv    the medians side by side

A page's figure is the median over its rounds; the first round is left
out when there are three or more, since it includes a cold start. The
total row adds the pages' medians.
"""
import statistics
import sys

COLS = ["load_ms", "first_paint_ms", "script_ms", "style_ms", "layout_ms", "paint_ms"]


def read(path):
    meta, rows, head = {}, [], None
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("# "):
                k, _, v = line[2:].partition(" ")
                meta[k] = v
            elif head is None:
                head = line.split("\t")
            elif line:
                rows.append(dict(zip(head, line.split("\t"))))
    pages = {}
    for r in rows:
        pages.setdefault(r["url"].rsplit("/", 1)[-1], []).append(r)
    med = {}
    for page, rs in pages.items():
        ok = [r for r in rs if r["status"] == "ok"]
        if len(ok) >= 3:
            ok = [r for r in ok if r["round"] != "1"]
        if not ok:
            med[page] = None
            continue
        med[page] = {c: statistics.median(int(r[c]) for r in ok) for c in COLS}
    return meta, med


def fmt(v):
    return "-" if v is None else ("%g" % v)


def main(argv):
    if len(argv) not in (2, 3):
        print(__doc__.strip(), file=sys.stderr)
        return 2
    runs = [read(p) for p in argv[1:]]
    for (meta, _), p in zip(runs, argv[1:]):
        print("%s: build %s, repeat %s" % (p, meta.get("build", "?"), meta.get("repeat", "?")))
    pages = sorted(set().union(*(m.keys() for _, m in runs)))
    width = max([len(p) for p in pages] + [5])
    if len(runs) == 1:
        med = runs[0][1]
        print("%-*s %s" % (width, "page", " ".join("%10s" % c[:-3] for c in COLS)))
        tot = {c: 0 for c in COLS}
        for p in pages:
            m = med[p]
            print("%-*s %s" % (width, p, " ".join("%10s" % fmt(m and m[c]) for c in COLS)))
            for c in COLS:
                tot[c] += m[c] if m else 0
        print("%-*s %s" % (width, "total", " ".join("%10s" % fmt(tot[c]) for c in COLS)))
        return 0
    old, new = runs[0][1], runs[1][1]
    print("%-*s %s" % (width, "page", " ".join("%20s" % c[:-3] for c in COLS)))
    tot_o = {c: 0 for c in COLS}
    tot_n = {c: 0 for c in COLS}

    def cell(a, b):
        if a is None or b is None:
            return "%20s" % ("%s > %s" % (fmt(a), fmt(b)))
        pct = "" if a == 0 else " %+.0f%%" % ((b - a) * 100.0 / a)
        return "%20s" % ("%g > %g%s" % (a, b, pct))

    for p in pages:
        o, n = old.get(p), new.get(p)
        print("%-*s %s" % (width, p, " ".join(cell(o and o[c], n and n[c]) for c in COLS)))
        if o and n:
            for c in COLS:
                tot_o[c] += o[c]
                tot_n[c] += n[c]
    print("%-*s %s" % (width, "total", " ".join(cell(tot_o[c], tot_n[c]) for c in COLS)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
