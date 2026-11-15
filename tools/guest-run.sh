#!/bin/sh
# Aura's own Mac OS 9.2.2 test machine: QEMU mac99, headless, in the
# background, with its own disk and QMP socket in AURA_RUN.
#
# Usage:
#   tools/guest-run.sh start   boot the guest (no window; returns at once)
#   tools/guest-run.sh stop    quit the guest started here
#   tools/guest-run.sh status  say whether it runs
#   tools/guest-run.sh reset   throw away the guest's disk changes
#   BASE=enforcer tools/guest-run.sh start   boot the Enforcer system
#
# MEM sets the RAM in megabytes (default 512). The guest's disk writes are
# discarded when QEMU exits (-snapshot), so every start boots the same
# system; PERSIST=1 keeps them in the overlay instead. Mac OS keeps the
# screen size and depth in its own preferences: change them in the guest
# (Control Strip or Monitors), not on the QEMU command line.
#
# BASE picks the system: stock (default, Mac OS 9.2.2 without Enforcer)
# or enforcer (with the os9-enforcer kernel and extension). The first
# start of a base flattens a private copy of its os9-protect image into
# AURA_RUN (reading it only), and boots an overlay on that copy, so
# nothing os9-protect owns is ever written or locked for writing. See
# tools/guest-env.sh for the settings.
set -eu
. "$(dirname "$0")/guest-env.sh"

running() {
  [ -f "$AURA_PID" ] && kill -0 "$(cat "$AURA_PID")" 2>/dev/null
}

prepare_disk() {
  if [ ! -f "$AURA_DISK_COPY" ]; then
    echo "guest-run: copying $AURA_BASE to $AURA_DISK_COPY (read-only use of the source)"
    # -U: do not take os9-protect's image lock; the source is only read,
    # and its own QEMU runs it with -snapshot, so nothing writes it.
    "$QEMU_IMG" convert -U -O qcow2 "$AURA_BASE" "$AURA_DISK_COPY.tmp"
    mv "$AURA_DISK_COPY.tmp" "$AURA_DISK_COPY"
  fi
  if [ ! -f "$AURA_DISK" ]; then
    "$QEMU_IMG" create -q -f qcow2 -F qcow2 -b "$AURA_DISK_COPY" "$AURA_DISK"
  fi
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
    # shellcheck disable=SC2086
    "$QEMU" \
      -name "Aura test machine" \
      -M mac99,via=pmu \
      -m "${MEM:-512}" \
      -boot c \
      -drive file="$AURA_DISK",format=qcow2,media=disk \
      -drive if=ide,index=2,media=cdrom \
      -g 1024x768x32 \
      -prom-env 'vga-ndrv?=true' \
      -prom-env 'auto-boot?=true' \
      -nic user,model=sungem \
      -qmp unix:"$AURA_QMP",server=on,wait=off \
      -display none $snapshot \
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
