#!/usr/bin/env sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
DEBIAN_DIR="$ROOT_DIR/Debian"
WORK_DIR=${DEBIAN_WORKDIR:-$(mktemp -d /tmp/kurono-debian.XXXXXX)}
ROOTFS_DIR="$WORK_DIR/debian-root"
ROOTFS_TAR_TMP="$WORK_DIR/debian-minbase.tar.gz"
ROOTFS_IMG_TMP="$WORK_DIR/debian-minbase.ext4"
ROOTFS_TAR="$DEBIAN_DIR/debian-minbase.tar.gz"
ROOTFS_IMG="$DEBIAN_DIR/debian-minbase.ext4"
MANIFEST="$DEBIAN_DIR/manifest.txt"

RELEASE=${1:-stable}
MIRROR=${2:-http://deb.debian.org/debian}
IMAGE_SIZE_MB=${DEBIAN_IMAGE_SIZE_MB:-256}

need_cmd() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "Missing required tool: $1" >&2
        exit 1
    }
}

need_cmd debootstrap
need_cmd mke2fs
need_cmd tar

SUDO=
if [ "$(id -u)" -ne 0 ]; then
    if command -v sudo >/dev/null 2>&1; then
        SUDO=sudo
    else
        echo "This script needs root privileges (or sudo) to run debootstrap." >&2
        exit 1
    fi
fi

mkdir -p "$DEBIAN_DIR"
rm -f "$ROOTFS_TAR" "$ROOTFS_IMG" "$MANIFEST"
$SUDO rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"

cleanup() {
    $SUDO rm -rf "$WORK_DIR"
}
trap cleanup EXIT INT TERM

echo "[debian-rootfs] Building Debian minbase rootfs..."
$SUDO debootstrap \
    --variant=minbase \
    --include=systemd-sysv,ifupdown,iproute2,isc-dhcp-client,netbase,ca-certificates,curl,wget \
    "$RELEASE" "$ROOTFS_DIR" "$MIRROR"

echo "[debian-rootfs] Applying Kurono guest configuration..."
$SUDO sh -c "cat > '$ROOTFS_DIR/etc/hostname' <<'EOF'
kurono-debian
EOF"

$SUDO mkdir -p "$ROOTFS_DIR/etc/systemd/system/serial-getty@ttyS0.service.d"
$SUDO sh -c "cat > '$ROOTFS_DIR/etc/systemd/system/serial-getty@ttyS0.service.d/autologin.conf' <<'EOF'
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin root --keep-baud 115200,38400,9600 %I vt102
Type=idle
EOF"

$SUDO sh -c "cat > '$ROOTFS_DIR/etc/network/interfaces' <<'EOF'
auto lo
iface lo inet loopback

auto eth0
allow-hotplug eth0
iface eth0 inet dhcp
EOF"

$SUDO mkdir -p "$ROOTFS_DIR/etc/systemd/network"
$SUDO sh -c "cat > '$ROOTFS_DIR/etc/systemd/network/20-eth0.network' <<'EOF'
[Match]
Name=eth0

[Network]
DHCP=yes
EOF"

$SUDO mkdir -p "$ROOTFS_DIR/usr/local/sbin"
$SUDO sh -c "cat > '$ROOTFS_DIR/usr/local/sbin/kurono-guest-init.sh' <<'EOF'
#!/bin/sh
ip link set lo up 2>/dev/null || true
ip link set eth0 up 2>/dev/null || true
dhclient eth0 2>/dev/null || ifup eth0 2>/dev/null || true
mkdir -p /var/lib/apt/lists/partial /tmp 2>/dev/null || true
exit 0
EOF"
$SUDO chmod +x "$ROOTFS_DIR/usr/local/sbin/kurono-guest-init.sh"

$SUDO mkdir -p "$ROOTFS_DIR/etc/systemd/system"
$SUDO sh -c "cat > '$ROOTFS_DIR/etc/systemd/system/kurono-guest-init.service' <<'EOF'
[Unit]
Description=Kurono guest bootstrap
After=network-pre.target
Before=getty.target serial-getty@ttyS0.service

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/kurono-guest-init.sh
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF"

$SUDO mkdir -p "$ROOTFS_DIR/etc/systemd/system/multi-user.target.wants"
$SUDO ln -sf /etc/systemd/system/kurono-guest-init.service \
    "$ROOTFS_DIR/etc/systemd/system/multi-user.target.wants/kurono-guest-init.service"
$SUDO ln -sf /lib/systemd/system/systemd-networkd.service \
    "$ROOTFS_DIR/etc/systemd/system/multi-user.target.wants/systemd-networkd.service" || true

$SUDO sh -c "printf 'nameserver 1.1.1.1\nnameserver 8.8.8.8\n' > '$ROOTFS_DIR/etc/resolv.conf'"
$SUDO sh -c "cat > '$ROOTFS_DIR/etc/motd' <<'EOF'
Kurono Debian minbase guest
Serial console: ttyS0 (root autologin enabled)
EOF"

$SUDO chroot "$ROOTFS_DIR" /bin/sh -lc "apt-get clean && rm -rf /var/lib/apt/lists/*"

echo "[debian-rootfs] Creating archive and ext4 disk image..."
$SUDO tar -C "$ROOTFS_DIR" -czf "$ROOTFS_TAR_TMP" .
$SUDO mke2fs -q -d "$ROOTFS_DIR" -t ext4 -L KURONO_DEBIAN -F "$ROOTFS_IMG_TMP" "${IMAGE_SIZE_MB}M"

$SUDO cp "$ROOTFS_TAR_TMP" "$ROOTFS_TAR"
$SUDO cp "$ROOTFS_IMG_TMP" "$ROOTFS_IMG"

du -sh "$ROOTFS_DIR" > "$MANIFEST"
printf 'release=%s\nmirror=%s\nimage=%s\n' "$RELEASE" "$MIRROR" "$ROOTFS_IMG" >> "$MANIFEST"

echo "[debian-rootfs] Ready:"
echo "  rootfs dir : $ROOTFS_DIR"
echo "  rootfs tar : $ROOTFS_TAR"
echo "  rootfs img : $ROOTFS_IMG"
echo "[debian-rootfs] Rebuild the Kurono ISO so the Debian image is embedded."
