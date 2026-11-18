#!/bin/sh
# Aura's own Mac OS 9.2.2 test machine: QEMU mac99, headless, in the
# background, with its own disk and QMP socket in AURA_RUN.
#
# Usage:
#   tools/guest-run.sh start   boot the guest (no window; returns at once)
#   tools/guest-run.sh stop    quit the guest started here
#   tools/guest-run.sh status  say whether it runs
#   tools/guest-run.sh reset   throw away the guest's disk changes
#   WINDOW=1 tools/guest-run.sh start        boot with a window, to use by hand
#
# MEM sets the RAM in megabytes (default 512). The guest's disk writes are
# discarded when QEMU exits (-snapshot), so every start boots the same
# system; PERSIST=1 keeps them in the overlay instead. Mac OS keeps the
# screen size and depth in its own preferences: change them in the guest
# (Control Strip or Monitors). GEOMETRY (default 1024x768x32) is the
# largest mode the display offers; GEOMETRY=1440x1050x32 allows the size
# of a large screen.
#
# The guest's clock starts at the local time of the base image's time
# zone (-rtc base; AURA_GUEST_UTC_OFFSET in tools/guest-env.sh), so Mac OS
# shows the right time for its zone and Aura works out the right UTC.
#
# QEMU_EXTRA adds arguments to QEMU's command line (tools/bench.sh passes
# -icount there).
#
# The guest boots stock Mac OS 9.2.2 (BASE=stock, the only base: Aura is
# not tested under Enforcer since 2026-11-18) through an overlay in
# AURA_RUN on the base image in disks/, so the base image is never
# written. An overlay is valid only for the base image it was made on:
# when the base image has changed since (an older image was restored,
# say), start and reset set the old
# overlay aside as os9-overlay-BASE.stale.qcow2 and make a new one. See
# tools/guest-env.sh for the settings.
set -eu
. "$(dirname "$0")/guest-env.sh"

running() {
  [ -f "$AURA_PID" ] && kill -0 "$(cat "$AURA_PID")" 2>/dev/null
}

prepare_disk() {
  if [ ! -f "$AURA_BASE" ]; then
    echo "guest-run: no base image $AURA_BASE (see docs/building.md, Disk images)" >&2
    exit 1
  fi
  stamp=$(base_stamp)
  if [ -f "$AURA_DISK" ]; then
    if [ -f "$AURA_DISK_STAMP" ]; then
      [ "$(cat "$AURA_DISK_STAMP")" = "$stamp" ] || stale=1
    elif [ "$AURA_BASE" -nt "$AURA_DISK" ]; then
      # An overlay from before stamps were written: the base is newer.
      stale=1
    fi
    if [ "${stale:-}" = 1 ]; then
      # Booting an overlay on a changed base damages the guest's disk.
      aside=$AURA_RUN/os9-overlay-$BASE.stale.qcow2
      mv -f "$AURA_DISK" "$aside"
      echo "guest-run: $AURA_BASE changed since the overlay was made;" \
        "the old overlay is now $aside" >&2
    fi
  fi
  if [ ! -f "$AURA_DISK" ]; then
    "$QEMU_IMG" create -q -f qcow2 -F qcow2 -b "$AURA_BASE" "$AURA_DISK"
  fi
  printf '%s\n' "$stamp" > "$AURA_DISK_STAMP"
}

# The base image's modification time and size, which identify the image
# an overlay was made on. GNU stat takes -c, BSD stat -f.
base_stamp() {
  stat -c '%Y %s' "$AURA_BASE" 2>/dev/null || stat -f '%m %z' "$AURA_BASE"
}

# The guest's local time now, for -rtc base: UTC plus the zone offset of
# the base image (AURA_GUEST_UTC_OFFSET in guest-env.sh). QEMU's default
# is UTC, which Mac OS would take for local time. GNU date takes -d,
# BSD date -r.
rtc_base() {
  t=$(($(date +%s) + AURA_GUEST_UTC_OFFSET))
  date -u -d "@$t" +%Y-%m-%dT%H:%M:%S 2>/dev/null || date -u -r "$t" +%Y-%m-%dT%H:%M:%S
}

case "${1:-}" in
  start)
    if running; then
      echo "guest-run: already running (pid $(cat "$AURA_PID"), base $(cat "$AURA_RUNNING_BASE" 2>/dev/null))"
      exit 0
    fi
    prepare_disk
    snapshot=-snapshot
    [ "${PERSIST:-}" = 1 ] && snapshot=
    rm -f "$AURA_QMP"
    if [ "${WINDOW:-}" = 1 ]; then
      # A window on the host's screen, for using the guest by hand. The
      # Cocoa display cannot daemonize, so QEMU runs as a background job.
      # shellcheck disable=SC2086
      "$QEMU" \
        -name "Aura test machine" \
        -M mac99,via=pmu \
        -m "${MEM:-512}" \
        -rtc base="$(rtc_base)" \
        -boot c \
        -drive file="$AURA_DISK",format=qcow2,media=disk \
        -drive if=ide,index=2,media=cdrom \
        -g "${GEOMETRY:-1024x768x32}" \
        -prom-env 'vga-ndrv?=true' \
        -prom-env 'auto-boot?=true' \
        -nic user,model=sungem \
        -qmp unix:"$AURA_QMP",server=on,wait=off \
        -display cocoa,zoom-integer=on $snapshot ${QEMU_EXTRA:-} \
        -pidfile "$AURA_PID" \
        >"$AURA_LOG" 2>&1 &
      i=0
      while [ ! -s "$AURA_PID" ] && [ $i -lt 50 ]; do sleep 0.2; i=$((i + 1)); done
      printf '%s\n' "$BASE" > "$AURA_RUNNING_BASE"
      echo "guest-run: started base $BASE in a window (pid $(cat "$AURA_PID")), QMP at $AURA_QMP"
      exit 0
    fi
    # shellcheck disable=SC2086
    "$QEMU" \
      -name "Aura test machine" \
      -M mac99,via=pmu \
      -m "${MEM:-512}" \
      -rtc base="$(rtc_base)" \
      -boot c \
      -drive file="$AURA_DISK",format=qcow2,media=disk \
      -drive if=ide,index=2,media=cdrom \
      -g "${GEOMETRY:-1024x768x32}" \
      -prom-env 'vga-ndrv?=true' \
      -prom-env 'auto-boot?=true' \
      -nic user,model=sungem \
      -qmp unix:"$AURA_QMP",server=on,wait=off \
      -display none $snapshot ${QEMU_EXTRA:-} \
      -daemonize -pidfile "$AURA_PID" \
      >"$AURA_LOG" 2>&1
    printf '%s\n' "$BASE" > "$AURA_RUNNING_BASE"
    echo "guest-run: started base $BASE (pid $(cat "$AURA_PID")), QMP at $AURA_QMP"
    ;;
  stop)
    if running; then
      "$(dirname "$0")/guest-qmp.py" quit || kill "$(cat "$AURA_PID")"
      echo "guest-run: stopped"
    else
      echo "guest-run: not running"
    fi
    rm -f "$AURA_PID" "$AURA_RUNNING_BASE"
    ;;
  status)
    if running; then
      echo "running (pid $(cat "$AURA_PID"), base $(cat "$AURA_RUNNING_BASE" 2>/dev/null))"
    else
      echo "not running"
      exit 1
    fi
    ;;
  reset)
    if running; then
      echo "guest-run: stop the guest first" >&2
      exit 1
    fi
    rm -f "$AURA_DISK"
    prepare_disk
    echo "guest-run: disk overlay of base $BASE reset"
    ;;
  *)
    echo "usage: $0 start|stop|status|reset" >&2
    exit 2
    ;;
esac
