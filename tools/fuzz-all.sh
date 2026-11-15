#!/bin/sh
# Run every fuzzer for a capped time and print what each did.
#
# Usage: tools/fuzz-all.sh [SECONDS [JOBS [SEED [BUILD]]]]
#
#   SECONDS  wall-clock cap for each fuzzer (default 600)
#   JOBS     how many run at once (default 4)
#   SEED     the seed given to each (default 1)
#   BUILD    the build tree (default build/asan, the sanitizer build:
#            see Build and test in CLAUDE.md)
#
# The fuzzers are copied to RUN/fuzz/bin first, so a rebuild during the
# run does not replace a program that is running (macOS kills it). Each
# one's output goes to RUN/fuzz/NAME.log and its current input to
# RUN/fuzz/NAME.input (with NAME.input.seed). RUN is run/ of this
# checkout, or FUZZ_RUN. The table at the end gives, for each fuzzer, the
# exit status, the iterations it reports, the distinct UBSan report sites
# (UBSan reports a site once and goes on, so a run can pass with reports)
# and the AddressSanitizer errors. Exit status 1 if any fuzzer failed or
# reported anything.
#
# FUZZ_ONLY="css layout" runs only the named fuzzers (the names are the
# table's first column). Under AddressSanitizer one allocation over 256 MB
# and a process over 4 GB are errors (ASAN_OPTIONS, unless it is set): no
# input here should need either.
set -u
cd "$(dirname "$0")/.."
secs=${1:-600}
jobs=${2:-4}
seed=${3:-1}
build=${4:-build/asan}
src=$(pwd)
run=${FUZZ_RUN:-$src/run}/fuzz
big=100000000

if [ ! -x "$build/tests/html/fuzz_html" ]; then
  echo "fuzz-all: no fuzzers in $build; build that tree first" >&2
  exit 2
fi
mkdir -p "$run/bin"

# name | program (under BUILD/tests) | arguments. SECS, SEED and BIG are
# replaced; a fuzzer without a SECONDS argument takes the cap from
# AURA_FUZZ_SECONDS, which is set for all of them.
all() {
  cat <<EOF
svg|svg/fuzz_svg|BIG SEED SECS tests/svg/corpus/*
image|image/fuzz_image|BIG SEED tests/image/vectors/*/*
image-png|image/fuzz_image|BIG SEED tests/image/vectors/png/*
image-jpeg|image/fuzz_image|BIG SEED tests/image/vectors/jpeg/*
image-gif|image/fuzz_image|BIG SEED tests/image/vectors/gif/*
image-webp|image/fuzz_image|BIG SEED tests/image/vectors/webp/*
image-bmp|image/fuzz_image|BIG SEED tests/image/vectors/bmp/*
font|paint/fuzz_font|BIG SEED SECS tests/paint/fonts/*.ttf tests/paint/fonts/*.woff tests/paint/fonts/*.woff2
webfont|page/fuzz_webfont|BIG SEED SECS
blocklist|blocklist/fuzz_blocklist|BIG SEED SECS
html|html/fuzz_html|BIG SEED SECS tests/page/samples/*.html tests/html/cases/*.dat
css|css/fuzz_css|BIG SEED SECS tests/page/samples/*.css tests/css/cases/*.txt
layout|page/fuzz_layout|BIG SEED SECS tests/page/samples/*.html tests/page/samples/*.css tests/page/samples/*.gif
page|page/fuzz_page|BIG SEED
dom|js/fuzz_dom|BIG SEED SECS
url|url/fuzz_url|BIG SEED
transfer|net/fuzz_transfer|BIG SEED SECS tests/codec/inflate-vectors/* tests/codec/brotli-vectors/*.br
net|net/fuzz_net|all BIG SEED assets/net/aura-roots.bin
net-aia|net/fuzz_net|aia BIG SEED
net-tls|net/fuzz_net|tls BIG SEED SECS
cookies|net/fuzz_cookies|BIG SEED
inflate|codec/fuzz_inflate|BIG SEED tests/codec/inflate-vectors/*
brotli|codec/fuzz_brotli|BIG SEED tests/codec/brotli-vectors/*.br
files|browser/fuzz_files|BIG SEED SECS
EOF
}

list() {
  if [ -z "${FUZZ_ONLY:-}" ]; then
    all
  else
    all | while IFS='|' read -r name prog args; do
      for want in $FUZZ_ONLY; do
        [ "$want" = "$name" ] && echo "$name|$prog|$args"
      done
    done
  fi
}

ASAN_OPTIONS=${ASAN_OPTIONS:-max_allocation_size_mb=256:hard_rss_limit_mb=4096}
export ASAN_OPTIONS

one() { # name program args
  name=$1
  prog=$2
  args=$(echo "$3" | sed -e "s/BIG/$big/" -e "s/SEED/$seed/" -e "s/SECS/$secs/" -e "s| tests/| $src/tests/|g" \
    -e "s| assets/| $src/assets/|g")
  bin="$run/bin/$(basename "$prog")"
  # Some fuzzers write files of their own (fuzz-svg-last.bin,
  # fuzz-slow-N.html) in the folder they run in: give each its own.
  mkdir -p "$run/cwd-$name"
  # shellcheck disable=SC2086
  (cd "$run/cwd-$name" &&
    AURA_FUZZ_SECONDS=$secs AURA_FUZZ_SAVE="$run/$name.input" nice "$bin" $args >"$run/$name.log" 2>&1
    echo "exit $?" >>"$run/$name.log")
}

list | while IFS='|' read -r name prog args; do
  cp "$build/tests/$prog" "$run/bin/" || exit 2
done

n=0
list | while IFS='|' read -r name prog args; do
  one "$name" "$prog" "$args" &
  n=$((n + 1))
  if [ $((n % jobs)) -eq 0 ]; then
    wait
  fi
done
wait
# The loop above runs in a subshell; wait for its children's logs.
while :; do
  missing=0
  for name in $(list | cut -d'|' -f1); do
    grep -q '^exit ' "$run/$name.log" 2>/dev/null || missing=1
  done
  [ $missing -eq 0 ] && break
  sleep 2
done

bad=0
printf '%-11s %-5s %-6s %-5s %s\n' fuzzer exit ubsan asan result
for name in $(list | cut -d'|' -f1); do
  log="$run/$name.log"
  code=$(sed -n 's/^exit //p' "$log" | tail -1)
  ub=$(grep -a 'runtime error' "$log" | sed -e 's/: runtime error.*//' | sort -u | wc -l | tr -d ' ')
  as=$(grep -a -c 'ERROR: AddressSanitizer' "$log")
  last=$(grep -a -v -e '^exit ' -e '^SUMMARY' -e 'runtime error' "$log" | grep -a 'iterations\|documents' | tail -1)
  printf '%-11s %-5s %-6s %-5s %s\n' "$name" "$code" "$ub" "$as" "$last"
  if [ "$code" != 0 ] || [ "$ub" != 0 ] || [ "$as" != 0 ]; then
    bad=1
  fi
done
[ $bad -eq 0 ] || echo "fuzz-all: failures or reports; see $run/NAME.log (and NAME.input for the last input)"
exit $bad
