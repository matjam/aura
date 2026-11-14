#!/bin/sh
# Fetch the HTML parser conformance suites into build/ (git-ignored):
#   build/html5lib-tests  html5lib-tests (tokenizer tests)
#   build/wpt-parsing     web-platform-tests html/syntax/parsing (the
#                         tree-construction .dat files, which moved from
#                         html5lib-tests to WPT)
# The suites are test data only; nothing from them is built or run.
set -eu
cd "$(dirname "$0")/../.."
mkdir -p build

if [ ! -d build/html5lib-tests ]; then
    git clone --depth 1 https://github.com/html5lib/html5lib-tests build/html5lib-tests
else
    git -C build/html5lib-tests pull --ff-only
fi

if [ ! -d build/wpt-parsing ]; then
    git clone --depth 1 --filter=blob:none --sparse https://github.com/web-platform-tests/wpt build/wpt-parsing
    git -C build/wpt-parsing sparse-checkout set html/syntax/parsing/resources
else
    git -C build/wpt-parsing pull --ff-only
fi
