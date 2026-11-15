# Settings shared by the guest scripts. Source it; do not run it.
#
# AURA_RUN     runtime folder (disk overlays, sockets, screenshots, HFS
#              images). Default: run/ in the main checkout of this repo, so
#              it survives git worktrees being removed.
# OS9_PROTECT  the os9-protect checkout whose disk images and QEMU build
#              are used, read-only (default ../os9-protect beside the main
#              checkout).
# BASE         which Mac OS 9.2.2 system the guest boots:
#                stock     disks/os9-novm.qcow2: the base install with
#                          virtual memory off, no Enforcer (default)
#                enforcer  disks/os9-vmoff.qcow2: virtual memory off, with
#                          the os9-enforcer kernel and extension installed
#              Each base has its own private copy and overlay in AURA_RUN.
# AURA_BASE    the os9-protect disk to copy, overriding the one BASE names
#              (give BASE a new name too, so the copy is not mixed up with
#              another base's).
# QEMU         the QEMU binary (default: ~/.local/opt/qemu-os9/bin, then
#              os9-protect's local build, then qemu-system-ppc from PATH).
# QEMU_IMG     qemu-img, found the same way.

aura_repo=$(cd "$(dirname "$0")/.." && pwd)
# The main checkout: the parent of the shared .git directory.
aura_main=$(cd "$(git -C "$aura_repo" rev-parse --path-format=absolute --git-common-dir 2>/dev/null || echo "$aura_repo/.git")/.." && pwd)

AURA_RUN=${AURA_RUN:-$aura_main/run}
OS9_PROTECT=${OS9_PROTECT:-$(cd "$aura_main/.." && pwd)/os9-protect}
BASE=${BASE:-stock}
if [ -z "${AURA_BASE:-}" ]; then
  case "$BASE" in
    stock) AURA_BASE=$OS9_PROTECT/disks/os9-novm.qcow2 ;;
    enforcer) AURA_BASE=$OS9_PROTECT/disks/os9-vmoff.qcow2 ;;
    *)
      echo "guest: unknown BASE '$BASE' (stock or enforcer; or set AURA_BASE)" >&2
      exit 2
      ;;
  esac
fi
AURA_QEMU_OPT=$HOME/.local/opt/qemu-os9/bin
if [ -z "${QEMU:-}" ]; then
  if [ -x "$AURA_QEMU_OPT/qemu-system-ppc" ]; then
    QEMU=$AURA_QEMU_OPT/qemu-system-ppc
  elif [ -x "$OS9_PROTECT/qemu/build/qemu-system-ppc" ]; then
    QEMU=$OS9_PROTECT/qemu/build/qemu-system-ppc
  else
    QEMU=qemu-system-ppc
  fi
fi
if [ -z "${QEMU_IMG:-}" ]; then
  if [ -x "$AURA_QEMU_OPT/qemu-img" ]; then
    QEMU_IMG=$AURA_QEMU_OPT/qemu-img
  elif [ -x "$OS9_PROTECT/qemu/build/qemu-img" ]; then
    QEMU_IMG=$OS9_PROTECT/qemu/build/qemu-img
  else
    QEMU_IMG=qemu-img
  fi
fi

# Aura's own files in AURA_RUN. One guest runs at a time, so the socket,
# pid and log are shared; the disks are per base.
AURA_DISK_COPY=$AURA_RUN/os9-base-$BASE.qcow2 # private flattened copy of AURA_BASE
AURA_DISK=$AURA_RUN/os9-aura-$BASE.qcow2      # overlay the guest writes to
AURA_QMP=$AURA_RUN/qmp.sock
AURA_PID=$AURA_RUN/qemu.pid
AURA_LOG=$AURA_RUN/qemu.log
AURA_RUNNING_BASE=$AURA_RUN/qemu.base # the BASE of the running guest
export AURA_RUN QMP_SOCK="$AURA_QMP"
mkdir -p "$AURA_RUN"
