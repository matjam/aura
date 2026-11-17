#!/bin/sh
# Build Aura's Linux packages: a .deb and an AppImage (docs/gtk.md, "Packaging").
#
# Usage: tools/package-linux.sh [OUTDIR]     (default: dist/linux)
#
# Environment:
#   BUILD_DIR      build tree (default build/package-linux)
#   APPIMAGE_EXTRACT_AND_RUN=1  lets an AppImage appimagetool run without FUSE
#   APPIMAGETOOL   path of appimagetool; else appimagetool on PATH. When
#                  neither exists the AppDir is built and the AppImage step
#                  is skipped with a message.
#
# GTK4 and fontconfig are never bundled: the .deb depends on the distribution's
# libgtk-4-1 and libfontconfig1, and the AppImage uses the host's GTK4.
set -eu
cd "$(dirname "$0")/.."
root=$(pwd)
out=${1:-dist/linux}
build=${BUILD_DIR:-build/package-linux}
version=$(tools/version.sh)
case $(uname -m) in
    x86_64) arch=x86_64 ;;
    aarch64) arch=aarch64 ;;
    *) arch=$(uname -m) ;;
esac

mkdir -p "$out"
out=$(cd "$out" && pwd)

cmake -S . -B "$build" -G Ninja -DAURA_PLATFORM=gtk -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr >/dev/null
cmake --build "$build"

# The .deb (CPack, packaging/linux/CMakeLists.txt).
rm -rf "$build/cpack"
(cd "$build" && cpack -G DEB -B "$root/$build/cpack" >/dev/null)
mv "$build/cpack/aura_${version}_"*.deb "$out/"
echo "built $(ls "$out"/aura_"${version}"_*.deb)"

# The AppDir: the install tree, plus AppRun, the desktop file and the icon at
# its root.
appdir="$build/AppDir"
rm -rf "$appdir"
DESTDIR="$root/$appdir" cmake --install "$build" --strip >/dev/null
cat > "$appdir/AppRun" <<'EOF'
#!/bin/sh
here=$(dirname "$(readlink -f "$0")")
exec "$here/usr/bin/aura" "$@"
EOF
chmod 755 "$appdir/AppRun"
cp "$appdir/usr/share/applications/org.aura.Browser.desktop" "$appdir/"
cp "$appdir/usr/share/icons/hicolor/256x256/apps/org.aura.Browser.png" "$appdir/"
cp "$appdir/usr/share/icons/hicolor/256x256/apps/org.aura.Browser.png" "$appdir/.DirIcon"

tool=${APPIMAGETOOL:-}
[ -n "$tool" ] || tool=$(command -v appimagetool || true)
if [ -z "$tool" ]; then
    echo "appimagetool not found (set APPIMAGETOOL); AppDir left in $appdir, AppImage not built" >&2
    exit 0
fi
ARCH=$arch "$tool" "$appdir" "$out/Aura-$version-$arch.AppImage"
echo "built $out/Aura-$version-$arch.AppImage"
