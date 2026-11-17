#!/bin/sh
# Build Aura.
#
# Usage: ./build.sh [host|gtk|cocoa|ppc|test|all]
#   host  build the engine, host tools and tests for this machine (build/host)
#   test  build host, then run the tests
#   gtk   build the Linux GTK4 application (build/gtk/src/platform/gtk/aura)
#   cocoa build the macOS application (build/cocoa/src/platform/cocoa/Aura.app)
#   ppc   build the Mac OS 9 application with Retro68 (build/ppc), the
#         distributable folder and the self-extracting archive
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

gtk() {
    cmake -S . -B build/gtk -G Ninja -DAURA_PLATFORM=gtk >/dev/null
    cmake --build build/gtk
}

cocoa() {
    cmake -S . -B build/cocoa -G Ninja -DAURA_PLATFORM=cocoa >/dev/null
    cmake --build build/cocoa
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
    dist
    sea
}

# The distributable folder, build/ppc/dist/Aura: the application (as
# MacBinary) beside its Fonts folder (the bundled fonts, their licences
# and fonts.idx) and the root certificate store, aura-roots (Mozilla's
# root set from assets/net/aura-roots.bin, without the extension so that
# guest-push.sh does not take it for MacBinary). To push it to the guest:
#   tools/guest-push.sh -n AuraTest build/ppc/dist/Aura/*
dist() {
    d=build/ppc/dist/Aura
    rm -rf "$d"
    mkdir -p "$d/Fonts"
    cp build/ppc/src/platform/macos9/Aura.bin "$d/"
    cp assets/net/aura-roots.bin "$d/aura-roots"
    mkdir -p "$d/Licences"
    cp LICENSE "$d/Licences/Aura.txt"
    cp third_party/mbedtls/LICENSE "$d/Licences/MbedTLS.txt"
    cp third_party/quickjs/LICENSE "$d/Licences/QuickJS-ng.txt"
    for fam in assets/fonts/*/; do
        cp -R "$fam" "$d/Fonts/$(basename "$fam")"
    done
    cp assets/fonts/fonts.idx assets/fonts/README.md "$d/Fonts/"
    echo "$d: $(du -sk "$d/Fonts" | cut -f1) KB of fonts, $(du -sk "$d" | cut -f1) KB in all"
}

# The self-extracting archive of the distributable folder, in two forms:
# build/ppc/dist/Aura-VERSION.sea.bin (MacBinary) and Aura-VERSION.hqx
# (BinHex 4.0, plain text). Opened on Mac OS 9, it asks where to put the
# Aura folder and writes it there (src/platform/macos9/sea). Every date in
# it is the last commit's, or SOURCE_DATE_EPOCH when that is set, so the
# same commit gives the same files.
sea() {
    v=$(tools/version.sh)
    out=build/ppc/dist/Aura-$v
    date=${SOURCE_DATE_EPOCH:-$(git log -1 --format=%ct 2>/dev/null || date +%s)}
    python3 tools/make-sea.py --date "$date" build/ppc/dist/Aura "$out.sea.bin"
    python3 tools/binhex.py "$out.sea.bin" "$out.hqx"
    python3 tools/binhex.py --compare "$out.hqx" "$out.sea.bin"
}

case "${1:-all}" in
    host) host ;;
    test) run_tests ;;
    gtk) gtk ;;
    cocoa) cocoa ;;
    ppc) ppc ;;
    all) run_tests; ppc ;;
    *) echo "usage: $0 [host|gtk|cocoa|ppc|test|all]" >&2; exit 2 ;;
esac
