#!/bin/sh
# Copy files from the host into Aura's running guest.
#
# Usage: tools/guest-push.sh [-n "Volume Name"] file...
#
# Builds an HFS volume holding the files and inserts it as the guest's CD;
# Mac OS mounts it on the desktop. hcopy's automatic mode decides how each
# file is copied: .bin files are decoded as MacBinary (keeping resource
# forks, type and creator), .hqx as BinHex, .txt as text with Mac line
# endings, and anything else as raw data.
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
bytes=$(cat "$@" | wc -c)
mb=$(( (bytes + bytes / 4) / 1048576 + 2 ))
dd if=/dev/zero of="$image" bs=1048576 count="$mb" 2>/dev/null

# hfsutils keeps its current volume in $HOME/.hcwd; a private HOME keeps
# this from mixing with any other hfsutils use (os9-protect's push.sh).
HOME=$AURA_RUN hformat -l "$name" "$image" >/dev/null
for f in "$@"; do
  HOME=$AURA_RUN hcopy -a "$f" :
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
