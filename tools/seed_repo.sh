#!/bin/bash
set -e
DOC=/var/www/html
sudo -n mkdir -p "$DOC/packages" "$DOC/kurono/packages/kurono-core" "$DOC/kurono/packages/kurono-shell" "$DOC/kurono/packages/kurono-net"

cat <<'PKG' | sudo -n tee "$DOC/packages/Packages" >/dev/null
Package: kurono-core
Version: 1.0.0
Section: base
Description: Kurono core runtime libraries
Filename: kurono/packages/kurono-core/payload.pkg
Size: 32
Installed-Size: 64

Package: kurono-shell
Version: 1.0.0
Section: shell
Description: Kurono interactive shell
Filename: kurono/packages/kurono-shell/payload.pkg
Size: 32
Installed-Size: 48

Package: kurono-net
Version: 1.0.1
Section: net
Description: Kurono networking utilities
Filename: kurono/packages/kurono-net/payload.pkg
Size: 32
Installed-Size: 40
PKG

# Mirror at the alternate paths the kernel tries.
sudo -n mkdir -p "$DOC/repo" "$DOC/dists/stable/main/binary-amd64"
sudo -n cp "$DOC/packages/Packages" "$DOC/repo/Packages"
sudo -n cp "$DOC/packages/Packages" "$DOC/dists/stable/main/binary-amd64/Packages"
sudo -n cp "$DOC/packages/Packages" "$DOC/Packages"

for p in kurono-core kurono-shell kurono-net; do
  printf 'KPKG\x01%-28s' "$p" | sudo -n tee "$DOC/kurono/packages/$p/payload.pkg" >/dev/null
done

sudo -n chown -R www-data:www-data "$DOC"
sudo -n systemctl reload nginx
echo OK
