#!/bin/sh
# The page benchmark in the guest: loads every page of a corpus in Aura
# on Mac OS 9 and saves what each load cost (src/browser/bench.h).
#
# Usage: tools/bench.sh [NAME]
#
# It serves the corpus with tools/bench-serve.py, boots a headless guest
# in its own run folder, pushes build/ppc/dist/Aura, launches Aura and
# types about:bench?http://10.0.2.2:PORT/manifest.txt in the address
# field. Aura loads each page REPEAT times and posts its results to the
# server, which writes run/bench/NAME.tsv (default:
# DATE-COMMIT-BASE[-icount].tsv). Then it quits Aura and stops the guest.
# Compare two runs with tools/bench-compare.py OLD.tsv NEW.tsv.
#
# Environment:
#   BENCH_ROOT     the corpus (default tests/page/samples)
#   BENCH_MANIFEST a manifest file instead of every page of BENCH_ROOT
#   BENCH_REPEAT   loads of each page (default 3)
#   BENCH_SETTLE   milliseconds after each load before it is measured (default 1000)
#   BENCH_ICOUNT   QEMU -icount shift (default 0: the guest's clock counts
#                  instructions, one per nanosecond, so the times depend
#                  on the code run and not on the host's speed or load);
#                  "off" runs in real time
#   BENCH_TIMEOUT  seconds to wait for the results (default 3600)
#   BENCH_PORT     the server's port (default 8642)
#   BASE           the guest system (default stock: what users run)
#   AURA_RUN       the run folder (default run-bench in the main checkout;
#                  it must not hold a running guest)
#   BENCH_KEEP=1   leave the guest running at the end
set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/.." && pwd)
main=$(cd "$(git -C "$repo" rev-parse --path-format=absolute --git-common-dir 2>/dev/null || echo "$repo/.git")/.." && pwd)
AURA_RUN=${AURA_RUN:-$main/run-bench}
export AURA_RUN
BASE=${BASE:-stock}
export BASE
qmp="$here/guest-qmp.py"
app="$repo/build/ppc/dist/Aura"
port=${BENCH_PORT:-8642}
icount=${BENCH_ICOUNT:-0}
commit=$(git -C "$repo" rev-parse --short HEAD 2>/dev/null || echo unknown)
git -C "$repo" diff --quiet HEAD 2>/dev/null || commit="$commit+"
suffix=
[ "$icount" != off ] && suffix=-icount
name=${1:-$(date +%Y-%m-%d)-$commit-$BASE$suffix}
out="$main/run/bench/$name.tsv"

say() { echo "bench: $*"; }

if [ ! -f "$app/Aura.bin" ]; then
  say "no $app/Aura.bin; run ./build.sh ppc first"
  exit 1
fi
mkdir -p "$AURA_RUN" "$main/run/bench"
if "$here/guest-run.sh" status >/dev/null 2>&1; then
  say "a guest is already running in $AURA_RUN; stop it or set AURA_RUN"
  exit 1
fi
rm -f "$out"

started=0
server=
finish() {
  status=$1
  [ -n "$server" ] && kill "$server" 2>/dev/null
  if [ "$started" = 1 ] && [ "${BENCH_KEEP:-}" != 1 ]; then
    "$here/guest-run.sh" stop >/dev/null 2>&1
  fi
  exit "$status"
}
fail() {
  say "FAIL: $*"
  "$qmp" screenshot "$AURA_RUN/bench-failure.png" >/dev/null 2>&1
  finish 1
}
trap 'finish 1' INT TERM

set -- --port "$port" --root "${BENCH_ROOT:-$repo/tests/page/samples}" --out "$out" --once \
  --repeat "${BENCH_REPEAT:-3}" --settle "${BENCH_SETTLE:-1000}"
[ -n "${BENCH_MANIFEST:-}" ] && set -- "$@" --manifest "$BENCH_MANIFEST"
python3 "$here/bench-serve.py" "$@" &
server=$!

extra=
[ "$icount" != off ] && extra="-icount shift=$icount,sleep=off"
say "starting a $BASE guest in $AURA_RUN${extra:+ ($extra)}"
QEMU_EXTRA=$extra "$here/guest-run.sh" start >/dev/null || fail "the guest did not start"
started=1
"$qmp" wait-app Finder 600 || fail "Mac OS did not reach the Finder in 600 s"
sleep 15 # the Finder is in front before it takes keys

say "pushing $app"
"$here/guest-push.sh" -n AuraTest "$app"/* >/dev/null || fail "guest-push.sh failed"
"$qmp" open AuraTest Aura >/dev/null || fail "Aura did not launch"
sleep 5
"$qmp" key cmd-l || fail "could not send Command-L"
sleep 0.5
"$qmp" type "about:bench?http://10.0.2.2:$port/manifest.txt" || fail "could not type the address"
"$qmp" key return

limit=${BENCH_TIMEOUT:-3600}
t=0
while [ ! -s "$out" ]; do
  if [ "$t" -ge "$limit" ]; then
    fail "no results within $limit s"
  fi
  if ! "$qmp" wait-app Aura 5 >/dev/null 2>&1; then
    fail "Aura is no longer the current application"
  fi
  sleep 10
  t=$((t + 15))
done
"$qmp" screenshot "$AURA_RUN/bench-done.png" >/dev/null 2>&1
"$qmp" key cmd-q
"$qmp" wait-app Finder 60 >/dev/null 2>&1 || say "Aura did not quit to the Finder"
say "results in $out"
python3 "$here/bench-compare.py" "$out"
finish 0
