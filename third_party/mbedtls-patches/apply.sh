#!/bin/sh
# Rebuild third_party/mbedtls from a pristine Mbed TLS release.
#
# Usage: third_party/mbedtls-patches/apply.sh PRISTINE_DIR [DEST_DIR]
#
# PRISTINE_DIR is an unpacked Mbed TLS release tarball (for example
# mbedtls-4.1.1.tar.bz2 from the project's GitHub releases; it carries
# TF-PSA-Crypto, the cryptography library, in tf-psa-crypto/). The script
# copies the folders Aura builds into DEST_DIR (default:
# third_party/mbedtls), replacing what is there, then applies every
# third_party/mbedtls-patches/*.patch in name order with patch -p1.
# Left out: tests, programs, documentation, scripts, the framework, the
# release's build files, and the Everest, p256-m and post-quantum drivers.
# To update Mbed TLS: fetch the new release, check its SHA-256, run this
# script, fix any patch that no longer applies, update
# third_party/README.md, and run ./build.sh all.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
src=${1:?usage: apply.sh PRISTINE_DIR [DEST_DIR]}
dest=${2:-$here/../mbedtls}

dirs="library include
tf-psa-crypto/core tf-psa-crypto/dispatch tf-psa-crypto/extras
tf-psa-crypto/platform tf-psa-crypto/utilities tf-psa-crypto/include
tf-psa-crypto/drivers/builtin"
files="LICENSE README.md tf-psa-crypto/LICENSE tf-psa-crypto/README.md"

rm -rf "$dest"
mkdir -p "$dest/tf-psa-crypto/drivers"
for d in $dirs; do
    mkdir -p "$dest/$(dirname "$d")"
    cp -R "$src/$d" "$dest/$d"
done
for f in $files; do
    cp "$src/$f" "$dest/$f"
done
# The release's own build files are not used.
find "$dest" \( -name CMakeLists.txt -o -name Makefile -o -name '*.make' -o -name '.gitignore' \) -delete
for p in "$here"/*.patch; do
    [ -e "$p" ] || continue
    echo "applying $(basename "$p")"
    patch -s -p1 -d "$dest" < "$p"
done
