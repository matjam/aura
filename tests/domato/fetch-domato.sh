#!/bin/sh
# Fetch Domato (Google Project Zero's DOM fuzzer: a grammar-based
# generator of HTML, CSS and script) into build/domato (git-ignored), at
# the commit pinned below. Nothing of it is committed here; its licence
# is Apache 2.0. To move to another commit, change PIN and run again.
#
# Usage: tests/domato/fetch-domato.sh
set -eu
PIN=fadff396cc45d521cc594d3e2396e27e887b1963
URL=https://github.com/googleprojectzero/domato
cd "$(dirname "$0")/../.."
mkdir -p build
if [ -d build/domato/.git ] && [ "$(git -C build/domato rev-parse HEAD)" = "$PIN" ]; then
    echo "build/domato is at $PIN"
    exit 0
fi
rm -rf build/domato
git init -q build/domato
git -C build/domato remote add origin "$URL"
git -C build/domato fetch -q --depth 1 origin "$PIN"
git -C build/domato checkout -q FETCH_HEAD
echo "build/domato fetched at $(git -C build/domato rev-parse HEAD)"
