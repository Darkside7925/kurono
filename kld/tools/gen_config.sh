#!/bin/bash
# gen_config - configure the shared pinned tree under an exclusive lock and
# snapshot the generated headers into THIS worktree (kld/build/kgen).
#
# the three workers share one linux tree; whoever configured it last used to
# decide every worker's CONFIG_* view. the snapshot makes each worktree's build
# independent after this point. (satoru)
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
KVER=$(cat "$ROOT/kld/KLD_KERNEL_VERSION")
TREE=${LINUX:-/mnt/windows/Users/ibrah/kw/scratch/kld/linux-$KVER}
LOCK=/mnt/windows/Users/ibrah/kw/scratch/kld/linux-tree.lock
KGEN=${KLD_KGEN:-"$ROOT/kld/build/kgen"}
SRC="$ROOT/kld/config/kld.config"
# a family with its own overlay (kld/config/<family>.config, e.g. i915) gets
# its own snapshot (KLD_KGEN) from kld.config + the overlay; every other
# family keeps the shared one unchanged. (satoru)
if [ -n "${KLD_CONFIG_OVERLAY:-}" ]; then
    mkdir -p "$KGEN"
    # last line wins per symbol, as merge_config does. (satoru)
    cat "$SRC" "$KLD_CONFIG_OVERLAY" | awk '
        { k = $0; if ($0 ~ /^CONFIG_/) { sub(/=.*/, "", k) } else if ($0 ~ /^# CONFIG_.* is not set$/) { k = $2 } else { k = "" } }
        { line[NR] = $0; key[NR] = k; if (k != "") last[k] = NR }
        END { for (i = 1; i <= NR; i++) if (key[i] == "" || last[key[i]] == i) print line[i] }' > "$KGEN/merged.config"
    SRC="$KGEN/merged.config"
fi
SRCSHA=$(sha256sum "$SRC" | awk '{print $1}')
[ -d "$TREE/include/linux" ] || { echo "gen_config: no tree at $TREE" >&2; exit 1; }
mkdir -p "$KGEN"
if [ -f "$KGEN/source.sha256" ] && [ "$(cat "$KGEN/source.sha256")" = "$SRCSHA" ] \
   && [ -f "$KGEN/include/generated/autoconf.h" ]; then
    echo "[kgen] snapshot is current"
    exit 0
fi
exec 9>"$LOCK"
if ! flock -w 600 9; then
    echo "gen_config: timed out waiting for the linux-tree lock" >&2
    exit 1
fi
cd "$TREE"
echo "[kgen] configuring $TREE from a fresh x86_64_defconfig + kld.config"
# always start from defconfig: inheriting the previous worker's .config made
# the snapshot depend on history. (satoru)
make -s ARCH=x86_64 x86_64_defconfig >/dev/null
TMP=$(mktemp)
cp "$SRC" "$TMP"
scripts/kconfig/merge_config.sh -m .config "$TMP" >/dev/null
rm -f "$TMP"
make -s ARCH=x86_64 olddefconfig >/dev/null
make -s ARCH=x86_64 modules_prepare >/dev/null
mkdir -p "$KGEN/include" "$KGEN/arch/x86/include"
rm -rf "$KGEN/include/generated" "$KGEN/include/config" "$KGEN/arch/x86/include/generated"
cp -a include/generated "$KGEN/include/"
cp -a include/config "$KGEN/include/"
cp -a arch/x86/include/generated "$KGEN/arch/x86/include/"
cp .config "$KGEN/dot-config"
sha256sum "$KGEN/include/generated/autoconf.h" | awk '{print $1}' > "$KGEN/config.sha256"
printf '%s\n' "$SRCSHA" > "$KGEN/source.sha256"
echo "[kgen] snapshot $(cat "$KGEN/config.sha256")"
# end (satoru)
