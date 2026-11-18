#!/bin/sh
# Renders a URL with aura-host and with headless Chrome and compares them.
#
# Usage: tools/render-compare.sh URL [WIDTH] [HEIGHT] [OUTDIR]
#
# With no WIDTH, renders at a narrow viewport (400x900) and a wide one
# (1280x900). With WIDTH, renders at WIDTHxHEIGHT only (HEIGHT defaults
# to 900). For each width W it writes OUTDIR/aura-W.png,
# OUTDIR/chrome-W.png, OUTDIR/side-W.png (Aura left, Chrome right) and
# OUTDIR/diff-W.png, and prints the png-compare figures. OUTDIR defaults
# to run/compare.
#
# Environment: AURA_HOST (default build/host/src/platform/host/aura-host),
# CHROME (default google-chrome-stable), AURA_WAIT in milliseconds
# (default 4000), TIMEOUT in seconds for each render (default 120).
# See docs/developer-tools.md.
set -eu

if [ $# -lt 1 ] || [ $# -gt 4 ]; then
    sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
fi

root=$(cd "$(dirname "$0")/.." && pwd)
url=$1
height=${3:-900}
outdir=${4:-run/compare}
aura=${AURA_HOST:-$root/build/host/src/platform/host/aura-host}
chrome=${CHROME:-google-chrome-stable}
mac_chrome="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
if [ -z "${CHROME:-}" ] && ! command -v "$chrome" >/dev/null 2>&1 && [ -x "$mac_chrome" ]; then
    chrome=$mac_chrome
fi
wait_ms=${AURA_WAIT:-4000}
load_ms=${AURA_LOAD:-20000}
limit=${TIMEOUT:-120}

if [ $# -ge 2 ]; then
    widths=$2
else
    widths="400 1280"
fi

if [ ! -x "$aura" ]; then
    echo "render-compare: $aura not found; run ./build.sh first" >&2
    exit 1
fi
if ! command -v "$chrome" >/dev/null 2>&1; then
    echo "render-compare: $chrome not found; set CHROME" >&2
    exit 1
fi

mkdir -p "$outdir"

width_js="document.documentElement.scrollWidth+'/'+document.documentElement.clientWidth"
status=0
for w in $widths; do
    a=$outdir/aura-$w.png
    c=$outdir/chrome-$w.png
    rm -f "$a" "$c"
    echo "== ${w}x$height"
    if ! timeout "$limit" "$aura" chrome --native-chrome --net -w "$w" -h "$height" \
        --load-ms "$load_ms" --tab "$url" --do "wait:$wait_ms" --do "js:$width_js" -o "$a" \
        >"$outdir/aura-$w.log" 2>&1; then
        echo "aura-host failed; see $outdir/aura-$w.log" >&2
        status=1
    fi
    # chrome-shot.py sets the viewport through the DevTools protocol:
    # --window-size gives a layout at least 500 pixels wide.
    if ! timeout "$limit" python3 "$root/tools/chrome-shot.py" --chrome "$chrome" --wait 2000 \
        --timeout "$limit" --eval "$width_js" "$url" "$w" "$height" "$c" >"$outdir/chrome-$w.log" 2>&1; then
        echo "Chrome failed; see $outdir/chrome-$w.log" >&2
        status=1
    fi
    aw=$(sed -n 's/^js =: //p' "$outdir/aura-$w.log" | tail -1)
    cw=$(sed -n 's/^eval .*: //p' "$outdir/chrome-$w.log" | tail -1)
    echo "page width (scrollWidth/clientWidth): aura ${aw:-?}, chrome ${cw:-?}"
    if [ -s "$a" ] && [ -s "$c" ]; then
        python3 "$root/tools/png-compare.py" --side "$a" "$c" "$outdir/side-$w.png" "$outdir/diff-$w.png"
        echo "wrote $a $c $outdir/side-$w.png $outdir/diff-$w.png"
    else
        status=1
    fi
done
exit $status
