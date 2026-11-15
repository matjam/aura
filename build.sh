#!/bin/sh
# Build Aura.
#
# Usage: ./build.sh [host|ppc|test|all]
#   host  build the engine, host tools and tests for this machine (build/host)
#   test  build host, then run the tests
#   ppc   build the Mac OS 9 application with Retro68 (build/ppc)
#   all   test, then ppc (default)
#
# The Retro68 PowerPC toolchain is found through RETRO68, or in os9-protect
# (OS9_PROTECT, default ../os9-protect).
set -eu
cd "$(dirname "$0")"

host() {
    cmake -S . -B build/host -G Ninja >/dev/null
    cmake --build build/host
}

run_tests() {
    host
    ctest --test-dir build/host --output-on-failure
}

ppc() {
    if [ -z "${RETRO68:-}" ]; then
        RETRO68=${OS9_PROTECT:-../os9-protect}/toolchain/build/toolchain
    fi
    tc=$RETRO68/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
    if [ ! -f "$tc" ]; then
        echo "No Retro68 PowerPC toolchain at $RETRO68. Set RETRO68 to its folder." >&2
        exit 1
    fi
    cmake -S . -B build/ppc -G Ninja -DCMAKE_TOOLCHAIN_FILE="$tc" >/dev/null
    cmake --build build/ppc
    # Retro68 writes the MacBinary with every Finder flag clear. The Finder
    # shows the application's icon only when hasBundle is set. On a volume
    # with no desktop database (the disc guest-push.sh makes) it shows the
    # icon only from a custom icon: the 'icns' at ID -16455 that
    # tools/make-icon.py --custom-icon writes, plus hasCustomIcon.
    python3 tools/finder-flags.py --bundle --custom-icon build/ppc/src/platform/macos9/Aura.bin
}

case "${1:-all}" in
    host) host ;;
    test) run_tests ;;
    ppc) ppc ;;
    all) run_tests; ppc ;;
    *) echo "usage: $0 [host|ppc|test|all]" >&2; exit 2 ;;
esac
