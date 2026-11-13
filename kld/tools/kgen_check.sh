#!/bin/bash
# verify the KGEN snapshot against kld/config/kld.config. (satoru)
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
KGEN=${KLD_KGEN:-"$ROOT/kld/build/kgen"}
A="$KGEN/include/generated/autoconf.h"
CFG="$ROOT/kld/config/kld.config"
[ -n "${KLD_CONFIG_OVERLAY:-}" ] && CFG="$KGEN/merged.config"   # see gen_config.sh (satoru)
[ -f "$A" ] || { echo "kgen_check: no autoconf snapshot" >&2; exit 1; }
ALLOW="$ROOT/kld/tools/kgen_allow.txt"
allowed() { [ -f "$ALLOW" ] && grep -qx "$1" "$ALLOW"; }
bad=0
while IFS= read -r line; do
    case "$line" in
        CONFIG_*=y)
            k=${line%%=*}
            grep -q "^#define $k 1$" "$A" || allowed "$k" || { echo "[kgen] missing $k=1"; bad=1; } ;;
        CONFIG_*=m)
            k=${line%%=*}
            grep -q "^#define ${k}_MODULE 1$" "$A" || allowed "$k" || { echo "[kgen] missing ${k}_MODULE=1"; bad=1; } ;;
        CONFIG_*=n)
            k=${line%%=*}
            grep -q "^#define $k " "$A" && { echo "[kgen] $k should be off"; bad=1; } ;;
        '# CONFIG_'*' is not set')
            k=$(echo "$line" | sed 's/^# \(CONFIG_[A-Za-z0-9_]*\) is not set$/\1/')
            grep -q "^#define $k " "$A" && { echo "[kgen] $k should be off"; bad=1; } ;;
    esac
done < "$CFG"
[ "$bad" = 0 ] || { echo "kgen_check: config snapshot does not match kld.config" >&2; exit 1; }
exit 0
# end (satoru)
