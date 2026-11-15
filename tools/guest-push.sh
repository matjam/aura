#!/bin/sh
# Copy files from the host into Aura's running guest.
#
# Usage: tools/guest-push.sh [-n "Volume Name"] file...
#
# Builds an HFS volume holding the files and inserts it as the guest's CD;
# Mac OS mounts it on the desktop. .bin files are decoded as MacBinary
# (keeping resource forks, type and creator) and .hqx as BinHex. .html,
# .css, .js and .svg files are copied byte for byte as Aura documents
# (TEXT/AURA). .ttf and .ttc files become type tfil, creator AURA
# (data-fork TrueType for Aura); .idx files are copied byte for byte as
# SimpleText documents; licences, READMEs, .txt and .md files become
# SimpleText documents with Mac line endings. Anything else is copied as
# hcopy's automatic mode decides.
#
# A folder argument is copied as a folder, with its subfolders. To push the
# distributable folder (the application beside its Fonts folder), pass
# its contents: tools/guest-push.sh -n AuraTest build/ppc/dist/Aura/*.
# HFS names are at most 31 characters.
set -eu
. "$(dirname "$0")/guest-env.sh"
qmp="$(dirname "$0")/guest-qmp.py"

name=Transfer
if [ "${1:-}" = -n ]; then
  name=$2
  shift 2
fi
if [ $# -eq 0 ]; then
  echo "usage: $0 [-n \"Volume Name\"] file..." >&2
  exit 2
fi

# A new file name each time, so the guest sees a different disc.
image="$AURA_RUN/push-$(date +%Y%m%d-%H%M%S).img"

# Size: the files plus a quarter for HFS overhead, at least 2 MB, in whole
# megabytes (a whole number of 2048-byte CD sectors).
bytes=$(find "$@" -type f -exec cat {} + | wc -c)
mb=$(( (bytes + bytes / 4) / 1048576 + 2 ))
dd if=/dev/zero of="$image" bs=1048576 count="$mb" 2>/dev/null

# hfsutils keeps its current volume in $HOME/.hcwd; a private HOME keeps
# this from mixing with any other hfsutils use on this machine.
HOME=$AURA_RUN hformat -l "$name" "$image" >/dev/null

# Copies folder $1 into the HFS folder $2 (a path ending in ':').
copy_dir() {
  dest="$2$(basename "$1"):"
  HOME=$AURA_RUN hmkdir "$dest"
  for entry in "$1"/*; do
    [ -e "$entry" ] || continue
    base=$(basename "$entry")
    if [ ${#base} -gt 31 ]; then
      echo "guest-push: $entry: name longer than 31 characters" >&2
      exit 1
    fi
    if [ -d "$entry" ]; then
      # A subshell, so the recursion keeps this level's variables.
      ( copy_dir "$entry" "$dest" ) || exit 1
      continue
    fi
    copy_file "$entry" "$dest"
  done
}

# Copies file $1 into the HFS folder $2.
copy_file() {
  base=$(basename "$1")
  case "$base" in
    *.ttf|*.ttc)
      HOME=$AURA_RUN hcopy -r "$1" "$2$base"
      HOME=$AURA_RUN hattrib -t tfil -c AURA "$2$base" ;;
    *.idx)
      # Raw: Aura reads either line ending.
      HOME=$AURA_RUN hcopy -r "$1" "$2$base"
      HOME=$AURA_RUN hattrib -t TEXT -c ttxt "$2$base" ;;
    LICENSE*|OFL*|README*|*.txt|*.md)
      HOME=$AURA_RUN hcopy -t "$1" "$2$base"
      HOME=$AURA_RUN hattrib -t TEXT -c ttxt "$2$base" ;;
    *.html|*.htm|*.css|*.js|*.svg)
      # Raw: text mode would translate UTF-8 bytes as Latin-1.
      HOME=$AURA_RUN hcopy -r "$1" "$2$base"
      HOME=$AURA_RUN hattrib -t TEXT -c AURA "$2$base" ;;
    *)
      HOME=$AURA_RUN hcopy -a "$1" "$2" ;;
  esac
}

for f in "$@"; do
  if [ -d "$f" ]; then
    ( copy_dir "$f" ":" ) || exit 1
  else
    copy_file "$f" ":"
  fi
done
HOME=$AURA_RUN hls -l
HOME=$AURA_RUN humount

# Mac OS locks the tray while a disc is mounted and does not notice a disc
# swapped underneath it, so a mounted disc is ejected from the guest first.
last="$AURA_RUN/push-last-name"
case "$("$qmp" cd-state)" in
  locked*)
    "$qmp" guest-eject "$(cat "$last" 2>/dev/null || echo Transfer)"
    ;;
esac

"$qmp" change-cd "$image"
printf '%s\n' "$name" > "$last"
for old in "$AURA_RUN"/push-*.img; do
  [ "$old" = "$image" ] || rm -f "$old"
done
echo "pushed $# file(s) as \"$name\" ($image)"
