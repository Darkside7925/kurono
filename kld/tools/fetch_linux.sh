#!/bin/bash
# fetch_linux - download, configure and prepare the pinned kernel tree, then
# vendor the headers KLD builds against.
#
# WHY a configured tree is required (learned the hard way, do not shortcut it):
# an out-of-tree module build needs headers that only exist AFTER configuration:
#   generated/autoconf.h    - from .config
#   generated/timeconst.h   - bc over kernel/time/timeconst.bc, HZ-derived
#   generated/bounds.h      - compiled from kernel/bounds.c (NR_PAGEFLAGS, ...)
#   generated/asm-offsets.h - compiled from arch/x86/kernel/asm-offsets.c
#   asm/ wrappers           - the mandatory-y / generic-y mechanism
# Several of those are NUMERIC CONSTANTS derived from the config. Hand-writing
# them means silently guessing values that decide struct layout, so we run the
# kernel's own `make modules_prepare` and take its output verbatim.
#
# The configured tree is BUILD OUTPUT and stays outside the repo ($WORK).
# Only headers are vendored into third_party/, so the repo stays small and this
# script makes the whole thing reproducible from the version pin alone.
#
# Usage: tools/fetch_linux.sh [version]     (run from kld/)
# end (satoru)
set -eu
KVER="${1:-$(cat KLD_KERNEL_VERSION)}"
MAJ="${KVER%%.*}"
WORK="${KLD_WORK:-/var/tmp/kld}"
DST="../third_party/linux-$KVER"
TARBALL="linux-$KVER.tar.xz"
URL="https://cdn.kernel.org/pub/linux/kernel/v${MAJ}.x/$TARBALL"

mkdir -p "$WORK"
cd "$WORK"

if [ ! -f "$TARBALL" ]; then
  echo "[fetch] downloading $URL"
  curl -f -# -o "$TARBALL" "$URL"
fi

if [ ! -d "linux-$KVER/kernel" ]; then
  echo "[fetch] extracting full tree (needed for modules_prepare)"
  tar -xf "$TARBALL"
fi

cd "linux-$KVER"

if [ ! -f .config ]; then
  echo "[fetch] configuring (x86_64_defconfig)"
  make -s ARCH=x86_64 x86_64_defconfig >/dev/null
fi

# guard on a header ONLY modules_prepare produces. autoconf.h is created by
# defconfig itself, so guarding on it silently skips the prepare step and you
# end up missing bounds.h/timeconst.h/asm-offsets.h. (satoru)
if [ ! -f include/generated/bounds.h ]; then
  echo "[fetch] make modules_prepare (this is the step that generates headers)"
  make -s ARCH=x86_64 modules_prepare
fi

echo "[fetch] generated headers present:"
for h in autoconf.h timeconst.h bounds.h asm-offsets.h; do
  if [ -f "include/generated/$h" ]; then echo "         OK  $h"; else echo "         --  $h (absent)"; fi
done

# ── vendor headers only ─────────────────────────────────────────────────────
echo "[fetch] vendoring headers into $DST"
mkdir -p "$DST"
for d in include arch/x86/include; do
  mkdir -p "$DST/$d"
  cp -r "$d/." "$DST/$d/"
done
# the arch asm/ wrappers modules_prepare synthesized live here
if [ -d arch/x86/include/generated ]; then
  mkdir -p "$DST/arch/x86/include/generated"
  cp -r arch/x86/include/generated/. "$DST/arch/x86/include/generated/"
fi
# driver sources we build
mkdir -p "$DST/drivers/net/ethernet/intel"
cp -r drivers/net/ethernet/intel/e1000e "$DST/drivers/net/ethernet/intel/" 2>/dev/null || true
# imported upstream cores the families link in unmodified (kld/Makefile
# CORES_<driver>). (satoru)
for f in lib/dynamic_queue_limits.c; do
  mkdir -p "$DST/$(dirname "$f")"
  cp "$f" "$DST/$f"
done
cp COPYING Makefile "$DST/" 2>/dev/null || true

echo "[fetch] vendored size: $(du -sh "$DST" | awk '{print $1}')"
echo "[fetch] done - kernel $KVER ready"
