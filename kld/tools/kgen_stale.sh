#!/bin/bash
# delete a family's objects when its recorded config snapshot differs. (satoru)
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
KGEN=${KLD_KGEN:-"$ROOT/kld/build/kgen"}
CUR=$(cat "$KGEN/config.sha256")
for d in "$ROOT"/kld/build/*/; do
    [ -d "$d" ] || continue
    case "$(basename "$d")" in kgen|kgen-*) continue ;; esac
    # a family with its own snapshot is checked only against that one. (satoru)
    fam=$(basename "$d")
    if [ -n "${KLD_CONFIG_OVERLAY:-}" ]; then
        [ "$fam" = "${KLD_FAMILY:-}" ] || continue
    else
        [ -f "$ROOT/kld/config/$fam.config" ] && continue
    fi
    [ -f "$d/config.sha256" ] || continue
    old=$(cat "$d/config.sha256")
    if [ "$old" != "$CUR" ]; then
        fam=$(basename "$d")
        echo "[kgen] config changed, rebuilding $fam"
        rm -rf "$d"
    fi
done
# end (satoru)
