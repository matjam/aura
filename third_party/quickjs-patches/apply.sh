#!/bin/sh
# Rebuild third_party/quickjs from a pristine QuickJS-ng tree.
#
# Usage: third_party/quickjs-patches/apply.sh PRISTINE_DIR [DEST_DIR]
#
# Copies the engine files Aura builds (no CLI, no libc module) from
# PRISTINE_DIR (a release tarball or a clone of the release tag) into
# DEST_DIR (default: third_party/quickjs), then applies every
# third_party/quickjs-patches/*.patch in name order with patch -p1.
# To update QuickJS-ng: fetch the new release, run this script, fix any
# patch that no longer applies, and update third_party/README.md.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
src=${1:?usage: apply.sh PRISTINE_DIR [DEST_DIR]}
dest=${2:-$here/../quickjs}

files="LICENSE
quickjs.c quickjs.h quickjs-atom.h quickjs-opcode.h quickjs-c-atomics.h
libregexp.c libregexp.h libregexp-opcode.h
libunicode.c libunicode.h libunicode-table.h
dtoa.c dtoa.h cutils.h list.h
builtin-array-fromasync.h builtin-iterator-zip-keyed.h builtin-iterator-zip.h"

mkdir -p "$dest"
for f in $files; do
    cp "$src/$f" "$dest/$f"
done
for p in "$here"/*.patch; do
    [ -e "$p" ] || continue
    echo "applying $(basename "$p")"
    patch -s -p1 -d "$dest" < "$p"
done
