#!/bin/bash
# Build a real minimal Debian rootfs for Kurono and publish it under
# kurono.satorut.com path layout. Idempotent.
set -euo pipefail

REPO=/var/www/html
PKG_ROOT=$REPO/packages/debian
WORK=/tmp/kurono-debian-build
ROOTFS=$WORK/rootfs

sudo apt-get install -y debootstrap nginx jq coreutils >/dev/null 2>&1 || true

# Ensure nginx vhost serves the requested layout
sudo tee /etc/nginx/sites-available/kurono-satorut >/dev/null <<'NGINX'
server {
    listen 80 default_server;
    server_name kurono.satorut.com _;
    root /var/www/html;
    autoindex on;
    sendfile on;
    tcp_nopush on;
    location / { try_files $uri $uri/ =404; }
}
NGINX
sudo ln -sf /etc/nginx/sites-available/kurono-satorut /etc/nginx/sites-enabled/kurono-satorut
sudo rm -f /etc/nginx/sites-enabled/default /etc/nginx/sites-enabled/kurono
sudo nginx -t && sudo systemctl restart nginx

# Open port 80 if firewall is active
if command -v ufw >/dev/null && sudo ufw status | grep -qi active; then
    sudo ufw allow 80/tcp || true
    sudo ufw allow 443/tcp || true
fi
sudo iptables -C INPUT -p tcp --dport 80 -j ACCEPT 2>/dev/null || \
    sudo iptables -I INPUT -p tcp --dport 80 -j ACCEPT || true

mkdir -p "$WORK"

if [ ! -f "$PKG_ROOT/debian-rootfs-12.5-amd64.tar.gz" ]; then
    sudo rm -rf "$ROOTFS"
    sudo debootstrap --variant=minbase --arch=amd64 \
        --include=apt,bash,coreutils,procps,iproute2,ca-certificates,iputils-ping,nano,less,vim-tiny \
        bookworm "$ROOTFS" http://deb.debian.org/debian/

    # Configure rootfs as a Kurono guest image
    sudo tee "$ROOTFS/etc/hostname" >/dev/null <<<"kurono-debian"
    sudo tee "$ROOTFS/etc/hosts" >/dev/null <<EOF
127.0.0.1 localhost kurono-debian
::1       localhost ip6-localhost
EOF
    sudo tee "$ROOTFS/etc/apt/sources.list" >/dev/null <<EOF
deb http://deb.debian.org/debian bookworm main contrib non-free non-free-firmware
deb http://security.debian.org/debian-security bookworm-security main
deb http://deb.debian.org/debian bookworm-updates main
EOF
    sudo tee "$ROOTFS/etc/motd" >/dev/null <<EOF
Welcome to Debian (Kurono integration)
Kernel: Linux 6.8.0-kurono
EOF
    # Marker file Kurono can grep for to identify the rootfs
    sudo tee "$ROOTFS/etc/kurono-debian.json" >/dev/null <<EOF
{"version":"12.5","arch":"amd64","build":"$(date -u +%Y%m%dT%H%M%SZ)","kernel":"6.8.0-kurono"}
EOF

    # Build tarball
    sudo tar --numeric-owner -C "$ROOTFS" -czf "$WORK/debian-rootfs-12.5-amd64.tar.gz" .
    sudo mkdir -p "$PKG_ROOT"
    sudo mv "$WORK/debian-rootfs-12.5-amd64.tar.gz" "$PKG_ROOT/"
fi

cd "$PKG_ROOT"
SIZE=$(stat -c '%s' debian-rootfs-12.5-amd64.tar.gz)
SHA=$(sha256sum debian-rootfs-12.5-amd64.tar.gz | awk '{print $1}')

sudo tee "$PKG_ROOT/manifest.json" >/dev/null <<EOF
{
  "name": "debian",
  "version": "12.5",
  "description": "Debian 12 (bookworm) minbase rootfs for Kurono integration",
  "url": "/packages/debian/debian-rootfs-12.5-amd64.tar.gz",
  "size": $SIZE,
  "sha256": "$SHA",
  "kind": "rootfs",
  "mount": "/debian",
  "kernel_identity": "Linux 6.8.0-kurono",
  "post_install": ["disable-alpine-driver-overrides", "redetect-drivers", "register-shell-env:debian"]
}
EOF

# Aggregate index includes debian as the headline integration package.
sudo tee "$REPO/packages/index.json" >/dev/null <<EOF
{
  "repo": "kurono.satorut.com",
  "version": 2,
  "packages": [
    {"name":"debian","version":"12.5","description":"Debian 12 integration rootfs"}
  ]
}
EOF
# Same content also at /index.json (legacy path the kernel still tries)
sudo cp "$REPO/packages/index.json" "$REPO/index.json"

# Self-test
echo
echo "=== Self-test ==="
curl -sI http://127.0.0.1/packages/debian/manifest.json | head -1
curl -s http://127.0.0.1/packages/debian/manifest.json | head -20
curl -sI http://127.0.0.1/packages/debian/debian-rootfs-12.5-amd64.tar.gz | head -2
echo
ls -lh "$PKG_ROOT"
