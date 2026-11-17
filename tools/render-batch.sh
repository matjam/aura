#!/bin/sh
# Compares many pages with Chrome: every story link on a listing page
# (Hacker News's front page by default), or the URLs in a file, each
# rendered by tools/render-compare.sh, then ranked by how much of the
# page differs.
#
# Usage: tools/render-batch.sh [-j JOBS] [-w WIDTH] [-h HEIGHT] [-o OUTDIR]
#                              [-n MAX] [LISTING-URL | -f FILE]
#
# LISTING-URL defaults to https://news.ycombinator.com/. -f FILE reads
# one URL per line instead (# starts a comment). JOBS defaults to 4,
# WIDTH to 1280, HEIGHT to 900, MAX to 30 pages, OUTDIR to run/batch.
# Each page's images go in OUTDIR/NN/ (render-compare.sh's files), and
# OUTDIR/report.md and OUTDIR/index.html list the pages, worst first,
# with their side-by-side images. The environment of render-compare.sh
# (AURA_HOST, CHROME, AURA_WAIT, TIMEOUT) applies. See
# docs/developer-tools.md.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
jobs=4
width=1280
height=900
outdir=run/batch
max=30
file=
while [ $# -gt 0 ]; do
    case $1 in
        -j) jobs=$2; shift 2 ;;
        -w) width=$2; shift 2 ;;
        -h) height=$2; shift 2 ;;
        -o) outdir=$2; shift 2 ;;
        -n) max=$2; shift 2 ;;
        -f) file=$2; shift 2 ;;
        -*) sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
        *) break ;;
    esac
done
listing=${1:-https://news.ycombinator.com/}

mkdir -p "$outdir"
urls=$outdir/urls.txt
if [ -n "$file" ]; then
    grep -v '^[[:space:]]*#' "$file" | grep -v '^[[:space:]]*$' | head -n "$max" >"$urls"
else
    # The listing's outbound story links (Hacker News's titleline links;
    # on another site, every absolute http(s) link that leaves the site).
    curl -sL -A "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7)" "$listing" |
        python3 -c '
import sys, re, html, urllib.parse
src = sys.stdin.read()
base = sys.argv[1]
host = urllib.parse.urlparse(base).netloc
links = re.findall(r"<span class=\"titleline\"><a href=\"([^\"]+)\"", src)
if not links:
    links = re.findall(r"<a [^>]*href=\"(https?://[^\"]+)\"", src)
seen = set()
for l in links:
    u = urllib.parse.urljoin(base, html.unescape(l))
    if not u.startswith("http") or urllib.parse.urlparse(u).netloc == host or u in seen:
        continue
    # PDFs and videos are not pages to compare.
    if re.search(r"\.(pdf|mp4|webm|zip|gz)(\?|$)", u, re.I):
        continue
    seen.add(u)
    print(u)
' "$listing" | head -n "$max" >"$urls"
fi
n=$(wc -l <"$urls" | tr -d ' ')
echo "render-batch: $n pages, $jobs at a time, ${width}x$height, into $outdir"

# One page: its folder is its line number.
i=0
while IFS= read -r u; do
    i=$((i + 1))
    printf '%02d %s\n' "$i" "$u"
done <"$urls" | xargs -P "$jobs" -L 1 sh -c '
    # $0 OUTDIR, $1 ROOT, $2 WIDTH, $3 HEIGHT, then the line: $4 NN, $5 URL
    d="$0/$4"; mkdir -p "$d"
    printf "%s\n" "$5" >"$d/url.txt"
    "$1/tools/render-compare.sh" "$5" "$2" "$3" "$d" >"$d/compare.log" 2>&1 || true
    echo "  done $4 $5"
' "$outdir" "$root" "$width" "$height" || true

# The report, worst first: the share of pixels that differ by more than 8.
python3 - "$outdir" "$width" <<'PY'
import os, re, sys, html
out, width = sys.argv[1], sys.argv[2]
rows = []
for d in sorted(os.listdir(out)):
    p = os.path.join(out, d)
    if not os.path.isdir(p) or not os.path.exists(os.path.join(p, "url.txt")):
        continue
    url = open(os.path.join(p, "url.txt")).read().strip()
    log = open(os.path.join(p, "compare.log")).read() if os.path.exists(os.path.join(p, "compare.log")) else ""
    m = re.search(r"mean diff ([\d.]+).*?([\d.]+)% of pixels differ", log)
    side = os.path.join(d, "side-%s.png" % width)
    if m and os.path.exists(os.path.join(out, side)):
        rows.append((float(m.group(2)), float(m.group(1)), d, url, side))
    else:
        rows.append((-1.0, 0.0, d, url, None))
rows.sort(key=lambda r: -r[0])
with open(os.path.join(out, "report.md"), "w") as f:
    f.write("| Differs | Mean | Page | Folder |\n|---|---|---|---|\n")
    for pct, mean, d, url, side in rows:
        f.write("| %s | %s | %s | %s |\n" % ("failed" if pct < 0 else "%.1f%%" % pct,
                                           "" if pct < 0 else "%.1f" % mean, url, d))
with open(os.path.join(out, "index.html"), "w") as f:
    f.write("<!doctype html><meta charset=utf-8><title>Aura and Chrome</title>"
            "<style>body{font:14px sans-serif;margin:16px}img{max-width:100%;border:1px solid #ccc}"
            "h2{font-size:15px;margin:24px 0 6px}</style><h1>Aura (left) and Chrome (right)</h1>")
    for pct, mean, d, url, side in rows:
        label = "failed to render" if pct < 0 else "%.1f%% of pixels differ, mean %.1f" % (pct, mean)
        f.write("<h2>%s &mdash; <a href=\"%s\">%s</a> (%s)</h2>" % (html.escape(label), html.escape(url),
                                                                    html.escape(url), d))
        if side:
            f.write("<img loading=lazy src=\"%s\">" % html.escape(side))
for pct, mean, d, url, side in rows:
    print("%8s  %s  %s" % ("failed" if pct < 0 else "%.1f%%" % pct, d, url))
print("wrote %s/report.md and %s/index.html" % (out, out))
PY
