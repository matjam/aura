#!/bin/sh
# Run a downloaded conformance suite, or skip (exit 77) if it is absent.
# Usage: run-suite.sh tokenizer|tree RUNNER BUILD_DIR [EXPECTED_FAILURES]
kind=$1
runner=$2
build=$3
xfail=${4:-}

case $kind in
tokenizer)
    dir=$build/html5lib-tests/tokenizer
    set -- "$dir"/*.test
    ;;
tree)
    dir=$build/wpt-parsing/html/syntax/parsing/resources
    set -- "$dir"/*.dat
    ;;
*)
    echo "unknown suite $kind" >&2
    exit 2
    ;;
esac

if [ ! -e "$1" ]; then
    echo "SKIPPED: $dir not found. Run tests/html/fetch-suite.sh to fetch the suite."
    exit 77
fi

if [ -n "$xfail" ]; then
    exec "$runner" -x "$xfail" "$@"
fi
exec "$runner" "$@"
