#!/bin/sh
# Smoke test of the built application in the guest. Run it after every
# merge, after ./build.sh ppc.
#
# Usage: tools/smoke.sh
#
# It boots a headless stock guest in its own run folder, pushes
# build/ppc/dist/Aura with two local pages, launches Aura, and opens:
#   1. a local sample page (tests/page/samples/hn.html),
#   2. tools/smoke/recursion.html, whose script recurses without end and
#      must end with a RangeError (a stack overflow there froze the guest),
#   3. a live page, https://news.ycombinator.com/.
# After each it waits for the window title the page gives, takes a
# screenshot and checks that Aura is still the current application (a
# crash leaves the Finder in front; a frozen guest or a system error
# never shows the next title). It then quits Aura, waits for the Finder
# and stops the guest. Exit status 0: every step passed; 1: a step
# failed (the message says which; the screenshots are kept).
#
# Environment:
#   AURA_RUN     the run folder (default: run-smoke in the main checkout;
#                it must not hold a running guest)
#   SMOKE_LIVE=0 skip the live page (no network)
#   SMOKE_KEEP=1 leave the guest running at the end
#   SMOKE_OUT    where the screenshots go (default: AURA_RUN)
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/.." && pwd)
main=$(cd "$(git -C "$repo" rev-parse --path-format=absolute --git-common-dir 2>/dev/null || echo "$repo/.git")/.." && pwd)
AURA_RUN=${AURA_RUN:-$main/run-smoke}
export AURA_RUN
out=${SMOKE_OUT:-$AURA_RUN}
qmp="$here/guest-qmp.py"
app="$repo/build/ppc/dist/Aura"

say() { echo "smoke: $*"; }

if [ ! -f "$app/Aura.bin" ]; then
  say "no $app/Aura.bin; run ./build.sh ppc first"
  exit 1
fi
mkdir -p "$AURA_RUN" "$out"
if "$here/guest-run.sh" status >/dev/null 2>&1; then
  say "a guest is already running in $AURA_RUN; stop it or set AURA_RUN"
  exit 1
fi

started=0
finish() {
  status=$1
  if [ "$started" = 1 ] && [ "${SMOKE_KEEP:-}" != 1 ]; then
    "$here/guest-run.sh" stop >/dev/null 2>&1
  fi
  if [ "$status" = 0 ]; then
    say "PASS (screenshots in $out)"
  else
    say "FAIL (screenshots in $out)"
  fi
  exit "$status"
}
fail() {
  say "FAIL: $*"
  "$qmp" screenshot "$out/smoke-failure.png" >/dev/null 2>&1
  finish 1
}

# Types an address into Aura's address field and waits for the window
# title the page gives.
open_page() { # address title seconds shot
  "$qmp" key cmd-l || fail "could not send Command-L"
  sleep 0.5
  "$qmp" type "$1" || fail "could not type $1"
  "$qmp" key return
  "$qmp" wait-window Aura "$2" "$3" || fail "$1 did not show the title \"$2\" within $3 s"
  sleep 2 # let it paint
  "$qmp" screenshot "$out/$4" >/dev/null || fail "no screenshot"
  "$qmp" wait-app Aura 10 || fail "Aura is not the current application after $1"
  say "$1: \"$2\" ($4)"
}

say "starting a stock guest in $AURA_RUN"
BASE=stock "$here/guest-run.sh" start >/dev/null || { say "FAIL: the guest did not start"; exit 1; }
started=1
"$qmp" wait-app Finder 240 || fail "Mac OS did not reach the Finder in 240 s"
sleep 15 # the Finder is in front before it takes keys

say "pushing $app"
"$here/guest-push.sh" -n AuraTest "$app"/* "$repo/tests/page/samples/hn.html" "$here/smoke/recursion.html" >/dev/null ||
  fail "guest-push.sh failed"
"$qmp" open AuraTest Aura >/dev/null || fail "Aura did not launch"
sleep 3
"$qmp" screenshot "$out/smoke-launch.png" >/dev/null

open_page "AuraTest:hn.html" "Sample News" 90 smoke-local.png
open_page "AuraTest:recursion.html" "recursion: caught RangeError" 90 smoke-recursion.png
if [ "${SMOKE_LIVE:-1}" != 0 ]; then
  # The first TLS handshake waits for the entropy pool.
  open_page "news.ycombinator.com" "Hacker News" 240 smoke-live.png
fi

"$qmp" key cmd-q
"$qmp" wait-app Finder 60 || fail "Aura did not quit to the Finder"
say "Aura quit"
finish 0
