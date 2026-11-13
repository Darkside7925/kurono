#!/bin/bash
# mount the pinned linux tree ext4 image at the path the KLD builds use.
#
# the host scratch filesystem is a Windows mount that refuses the three kernel
# files named aux.c/aux.h, so the tree lives in an ext4 image loop-mounted
# under scratch/kld. this script makes that mount survive a reboot; it is
# idempotent and does nothing when the tree is already mounted. (satoru)
#
#   usage: kld/tools/mount_linux_tree.sh [image] [mountpoint]
#
# defaults:
#   image      /mnt/windows/Users/ibrah/kw/scratch/kld/linuxfs.img
#   mountpoint /mnt/windows/Users/ibrah/kw/scratch/kld/linux-6.12.99
# end (satoru)
set -u

SCRATCH=${KLD_SCRATCH:-/mnt/windows/Users/ibrah/kw/scratch/kld}
IMG=${1:-$SCRATCH/linuxfs.img}
MNT=${2:-$SCRATCH/linux-6.12.99}

[ -f "$IMG" ] || { echo "mount_linux_tree: no image at $IMG" >&2; exit 2; }

if mountpoint -q "$MNT"; then
    echo "mount_linux_tree: already mounted at $MNT"
    exit 0
fi

mkdir -p "$MNT" || exit 2
sudo -n mount -o loop,rw "$IMG" "$MNT" || {
    echo "mount_linux_tree: mount failed (sudo -n required)" >&2
    exit 2
}
[ -f "$MNT/include/linux/kconfig.h" ] || {
    echo "mount_linux_tree: mounted, but no configured tree at $MNT" >&2
    exit 1
}
echo "mount_linux_tree: $IMG mounted at $MNT"

# end (satoru)
