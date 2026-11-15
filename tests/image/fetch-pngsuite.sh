#!/bin/sh
# Fetches PngSuite (Willem van Schaik's official PNG test suite, linked
# from libpng.org's pub/png/pngsuite page) into build/pngsuite and makes
# reference decodes (8-bit RGBA PAM, gamma not applied) with ImageMagick.
# The image tests use them when present (test image_png_suite) and skip
# cleanly otherwise. Nothing here is committed.
#
# usage: tests/image/fetch-pngsuite.sh   (from the repository root)
set -eu
dest=build/pngsuite
mkdir -p build/pngsuite-dl "$dest/ref"
if [ ! -f build/pngsuite-dl/PngSuite.tgz ]; then
    curl -sL -o build/pngsuite-dl/PngSuite.tgz http://www.schaik.com/pngsuite/PngSuite-2017jul19.tgz
fi
tar -xzf build/pngsuite-dl/PngSuite.tgz -C "$dest"
command -v magick >/dev/null || { echo "ImageMagick (magick) is needed for the references"; exit 1; }
for f in "$dest"/[!x]*.png; do
    b=$(basename "$f" .png)
    # ImageMagick keeps samples as stored (it records gAMA without
    # applying it), as Aura does.
    magick "$f" -depth 8 -alpha on "PAM:$dest/ref/$b.pam" 2>/dev/null || true
done
echo "$dest: $(ls "$dest"/*.png | wc -l) images, $(ls "$dest/ref" | wc -l) references"
