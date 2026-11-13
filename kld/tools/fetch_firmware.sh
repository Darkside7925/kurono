#!/bin/bash
# fetch the firmware named in kld/firmware.manifest from the host /lib/firmware
# into kld/firmware/ (gitignored), verifying size and sha256. no blob is ever
# committed. (satoru)
#
#   usage: kld/tools/fetch_firmware.sh [source_dir]
#   default source_dir: /lib/firmware
# end (satoru)
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=${1:-/lib/firmware}
DST="$ROOT/firmware"
MAN="$ROOT/firmware.manifest"

[ -f "$MAN" ] || { echo "fetch_firmware: no manifest at $MAN" >&2; exit 2; }
mkdir -p "$DST" || exit 2

rc=0
while read -r name size sha rest; do
    case "$name" in ''|'#'*) continue ;; esac
    src="$SRC/$name"
    dst="$DST/$name"
    if [ ! -e "$src" ]; then
        echo "fetch_firmware: missing host file $src" >&2
        rc=1
        continue
    fi
    cp -f "$src" "$dst" || { rc=1; continue; }
    got_size=$(stat -Lc%s "$dst" 2>/dev/null || echo 0)
    got_sha=$(sha256sum "$dst" | cut -d' ' -f1)
    if [ "$got_size" != "$size" ] || [ "$got_sha" != "$sha" ]; then
        echo "fetch_firmware: $name mismatch (size $got_size/$size sha $got_sha/$sha)" >&2
        rc=1
        continue
    fi
    echo "fetch_firmware: $name ok ($size bytes)"
done < "$MAN"

exit "$rc"

# end (satoru)
