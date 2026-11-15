#!/bin/sh
# Generate Domato pages and run them through the document view with
# script on, or skip (exit 77) when Domato has not been fetched.
#
# Usage: tests/domato/run-domato.sh RUNNER BUILD_DIR COUNT SEED SECONDS
#
#   RUNNER     the fuzz_domato program (build/asan/tests/js/fuzz_domato)
#   BUILD_DIR  the folder holding domato/ (build/ in the checkout); the
#              pages go to BUILD_DIR/domato-out/SEED
#   COUNT      pages to generate
#   SEED       seed for the generator and the runner
#   SECONDS    wall-clock cap for the runner
#
# To fetch Domato, run tests/domato/fetch-domato.sh. A page that fails is
# named in the runner's output; run it again alone with
#   RUNNER replay BUILD_DIR/domato-out/SEED/fuzz-N.html
runner=$1
build=$2
count=$3
seed=$4
secs=$5
here=$(cd "$(dirname "$0")" && pwd)
if [ ! -f "$build/domato/generator.py" ]; then
    echo "run-domato: no $build/domato (run tests/domato/fetch-domato.sh); skipped"
    exit 77
fi
out=$build/domato-out/$seed
rm -rf "$out"
python3 -I "$here/generate.py" "$build/domato" "$out" "$count" "$seed" >/dev/null || {
    echo "run-domato: the generator failed" >&2
    exit 1
}
exec "$runner" "$secs" "$seed" "$out"/*.html
