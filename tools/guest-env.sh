# Settings shared by the guest scripts. Source it; do not run it.
#
# AURA_RUN     runtime folder (disk overlay, sockets, screenshots, HFS
#              images). Default: run/ in the main checkout of this repo, so
#              it survives git worktrees being removed.
# OS9_PROTECT  the os9-protect checkout whose disk images and QEMU build
#              are used, read-only (default ../os9-protect beside the main
#              checkout).
# AURA_BASE    the os9-protect disk the guest is copied from (default
#              disks/os9-vmoff.qcow2: Mac OS 9.2.2, virtual memory off).
# QEMU         the QEMU binary (default: os9-protect's local build, else
#              qemu-system-ppc from PATH).

aura_repo=$(cd "$(dirname "$0")/.." && pwd)
# The main checkout: the parent of the shared .git directory.
aura_main=$(cd "$(git -C "$aura_repo" rev-parse --path-format=absolute --git-common-dir 2>/dev/null || echo "$aura_repo/.git")/.." && pwd)

AURA_RUN=${AURA_RUN:-$aura_main/run}
OS9_PROTECT=${OS9_PROTECT:-$(cd "$aura_main/.." && pwd)/os9-protect}
AURA_BASE=${AURA_BASE:-$OS9_PROTECT/disks/os9-vmoff.qcow2}
if [ -z "${QEMU:-}" ]; then
  if [ -x "$OS9_PROTECT/qemu/build/qemu-system-ppc" ]; then
    QEMU=$OS9_PROTECT/qemu/build/qemu-system-ppc
  else
    QEMU=qemu-system-ppc
  fi
fi
QEMU_IMG=${QEMU_IMG:-qemu-img}
if [ -x "$OS9_PROTECT/qemu/build/qemu-img" ]; then
  QEMU_IMG=$OS9_PROTECT/qemu/build/qemu-img
fi

# Aura's own files in AURA_RUN.
AURA_DISK_COPY=$AURA_RUN/os9-base.qcow2  # private flattened copy of AURA_BASE
AURA_DISK=$AURA_RUN/os9-aura.qcow2       # overlay the guest writes to
AURA_QMP=$AURA_RUN/qmp.sock
AURA_PID=$AURA_RUN/qemu.pid
AURA_LOG=$AURA_RUN/qemu.log
export AURA_RUN QMP_SOCK="$AURA_QMP"
mkdir -p "$AURA_RUN"
