#!/bin/sh
# Build Aura.
#
# Usage: ./build.sh [host|ppc|test|all]
#   host  build the engine, host tools and tests for this machine (build/host)
#   test  build host, then run the tests
#   ppc   build the Mac OS 9 application with Retro68 (build/ppc)
#   all   test, then ppc (default)
#
# The Retro68 PowerPC toolchain is found by toolchain/find.sh: RETRO68,
# else ~/.local/opt/retro68, where toolchain/build.sh installs it. A
# build/ppc folder keeps the toolchain it was first configured with;
# delete it to switch.
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
    . toolchain/find.sh
    tc=$RETRO68/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
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
