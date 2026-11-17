#!/bin/sh
# Builds the macOS application as a universal binary (arm64 and x86_64)
# and packs it as OUTDIR/Aura-VERSION-macos.zip.
#
# Usage: tools/package-macos.sh [OUTDIR]     (default dist/macos)
#
# The application is signed ad hoc (codesign -s -), not with a Developer
# ID, and is not notarized: after downloading it, open it with the
# Finder's Open command once (docs/cocoa.md, "Packaging").
set -eu
cd "$(dirname "$0")/.."
out=${1:-dist/macos}
build=build/package-macos
cmake -S . -B "$build" -G Ninja -DAURA_PLATFORM=cocoa -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 >/dev/null
cmake --build "$build"
app=$build/src/platform/cocoa/Aura.app
codesign --verify --deep "$app"
version=$(tools/version.sh)
mkdir -p "$out"
zip=$out/Aura-$version-macos.zip
rm -f "$zip"
# ditto keeps the bundle's symbolic links, permissions and signature.
ditto -c -k --keepParent "$app" "$zip"
lipo -archs "$app/Contents/MacOS/Aura"
echo "$zip: $(du -k "$zip" | cut -f1) KB"
