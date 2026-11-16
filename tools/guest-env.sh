# Settings shared by the guest scripts. Source it; do not run it.
#
# AURA_RUN     runtime folder (disk overlays, sockets, screenshots, HFS
#              images). Default: run/ in the main checkout of this repo, so
#              it survives git worktrees being removed. To run a second
#              guest beside the first, give it another folder.
# AURA_DISKS   folder of the base disk images. Default: disks/ in the main
#              checkout. The images are standalone (no backing file) and
#              git-ignored; docs/building.md says where they came from.
# BASE         which Mac OS 9.2.2 system the guest boots:
#                enforcer  os9-vmoff.qcow2: virtual memory off, with the
#                          Enforcer kernel and extension installed
#                          (default)
#                stock     os9-novm.qcow2: the base install with virtual
#                          memory off, no Enforcer
#              Each base has its own overlay in AURA_RUN.
# AURA_GUEST_UTC_OFFSET
#              seconds the base image's Mac OS time zone is east of UTC,
#              daylight saving hour included, as the image's Date & Time
#              control panel has it. guest-run.sh starts the guest's
#              hardware clock at UTC plus this, because Mac OS reads the
#              hardware clock as local time: the guest then shows the
#              right local time for its zone and Aura works out the right
#              UTC, whatever the host's zone is. Both images are set to
#              Cupertino with daylight saving on (UTC-7; the Clock section
#              of Develop > System Status in Aura shows the zone). Change
#              the value here if you change an image's zone.
# QEMU         the QEMU binary. Default: ~/.local/opt/qemu-os9/bin, where
#              qemu/build.sh installs it.
# QEMU_IMG     qemu-img, from the same folder.

aura_repo=$(cd "$(dirname "$0")/.." && pwd)
# The main checkout: the parent of the shared .git directory.
aura_main=$(cd "$(git -C "$aura_repo" rev-parse --path-format=absolute --git-common-dir 2>/dev/null || echo "$aura_repo/.git")/.." && pwd)

AURA_RUN=${AURA_RUN:-$aura_main/run}
AURA_DISKS=${AURA_DISKS:-$aura_main/disks}
BASE=${BASE:-enforcer}
case "$BASE" in
  stock)
    AURA_BASE=$AURA_DISKS/os9-novm.qcow2
    AURA_GUEST_UTC_OFFSET=${AURA_GUEST_UTC_OFFSET:--25200}
    ;;
  enforcer)
    AURA_BASE=$AURA_DISKS/os9-vmoff.qcow2
    AURA_GUEST_UTC_OFFSET=${AURA_GUEST_UTC_OFFSET:--25200}
    ;;
  *)
    echo "guest: unknown BASE '$BASE' (stock or enforcer)" >&2
    exit 2
    ;;
esac

QEMU_PREFIX=${QEMU_PREFIX:-$HOME/.local/opt/qemu-os9}
QEMU=${QEMU:-$QEMU_PREFIX/bin/qemu-system-ppc}
QEMU_IMG=${QEMU_IMG:-$QEMU_PREFIX/bin/qemu-img}
if [ ! -x "$QEMU" ]; then
  echo "guest: no QEMU at $QEMU. To install it, run qemu/build.sh." >&2
  exit 1
fi

# Aura's own files in AURA_RUN. One guest runs per AURA_RUN, so the
# socket, pid and log are shared; the overlay is per base.
AURA_DISK=$AURA_RUN/os9-overlay-$BASE.qcow2 # what the guest writes to
AURA_DISK_STAMP=$AURA_DISK.base # the base image the overlay was made on
AURA_QMP=$AURA_RUN/qmp.sock
AURA_PID=$AURA_RUN/qemu.pid
AURA_LOG=$AURA_RUN/qemu.log
AURA_RUNNING_BASE=$AURA_RUN/qemu.base # the BASE of the running guest
export AURA_RUN QMP_SOCK="$AURA_QMP"
mkdir -p "$AURA_RUN"
