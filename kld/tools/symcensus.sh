#!/bin/bash
# symcensus - the KLD progress bar and the single source of truth for phase 2.
#
# Classifies every symbol the driver needs from OUTSIDE itself:
#   IMPLEMENTED : provided for real by hand-written lx_emul
#   STUBBED     : provided by an auto-generated aborting stub  <- the work queue
#   MISSING     : nobody provides it; the container will not link
#
# Three subtleties this has to get right, each of which produced a wrong number
# on the first attempt:
#  1. A symbol undefined in phy.o but DEFINED in ich8lan.o is an intra-driver
#     cross-TU call, not an external dependency. Subtract driver-defined symbols
#     or the totals are inflated with the driver's own internals.
#  2. lx_stubs_gen.o is part of the emul objects, so naively counting "defined by
#     emul" as implemented would count every stub as done. Exclude it explicitly.
#  3. libc/compiler symbols (memcpy, __udivdi3) resolve at link and are not
#     driver-API gaps, so they are neither a gap nor an achievement.
#
# Usage: symcensus.sh <builddir>
# end (satoru)
set -u
BUILD="${1:-build}"
ROOT=$(cd "$(dirname "$0")/../.." && pwd)

if [ ! -d "$BUILD" ]; then
  echo "symcensus: no build dir '$BUILD' - run make first" >&2
  exit 1
fi

DRV_OBJS=$(find "$BUILD/driver" -name '*.o' 2>/dev/null)
# imported upstream cores (unmodified linux .c files linked into the family)
CORE_OBJS=$(find "$BUILD/core" -name '*.o' 2>/dev/null || true)
# hand-written shim only: everything in emul EXCEPT the generated stubs
EMUL_HAND=$(find "$BUILD/emul" -name '*.o' 2>/dev/null | grep -v 'lx_stubs_gen' || true)

if [ -z "$DRV_OBJS" ]; then
  echo "symcensus: no driver objects in $BUILD/driver - nothing to census"
  exit 0
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# every symbol referenced but not defined, across all driver objects
nm --undefined-only $DRV_OBJS 2>/dev/null \
  | awk '$1=="U"{print $2}' | sort -u > "$TMP/undef"

# (1) the driver's own definitions - intra-driver, not external
nm --defined-only $DRV_OBJS 2>/dev/null | awk '{print $3}' | sort -u > "$TMP/drvdef"

# libc + compiler builtins (3)
printf '%s\n' memcpy memset memmove memcmp strlen strcmp strncmp strcpy strncpy \
              strcat strchr strrchr strstr strnlen snprintf vsnprintf sprintf printf \
              puts malloc free calloc realloc abort exit _exit write read \
              __stack_chk_fail __stack_chk_guard __memcpy_chk __memset_chk \
              __udivdi3 __umoddi3 __divdi3 __moddi3 \
              _GLOBAL_OFFSET_TABLE_ \
  | sort -u > "$TMP/libc"

# true external surface = undefined, minus intra-driver, minus libc
comm -23 "$TMP/undef" "$TMP/drvdef" > "$TMP/x1"
comm -23 "$TMP/x1"    "$TMP/libc"   > "$TMP/needed"

# (2) hand-written implementations only
if [ -n "$EMUL_HAND" ]; then
  nm --defined-only $EMUL_HAND 2>/dev/null | awk '{print $3}' | sort -u > "$TMP/handdef"
else
  : > "$TMP/handdef"
fi

# generated stubs
if [ -f "$BUILD/lx_stubs_gen.c" ]; then
  grep -oE 'LX_STUB\([A-Za-z0-9_]+\)' "$BUILD/lx_stubs_gen.c" 2>/dev/null \
    | sed 's/LX_STUB(//; s/)//' | sort -u > "$TMP/stubs"
else
  : > "$TMP/stubs"
fi

# symbols the driver needs that an imported upstream core provides (satoru)
if [ -n "$CORE_OBJS" ]; then
  nm --defined-only $CORE_OBJS 2>/dev/null | awk '{print $3}' | sort -u > "$TMP/coredef"
else
  : > "$TMP/coredef"
fi
comm -12 "$TMP/needed" "$TMP/coredef" > "$TMP/bycore"
comm -23 "$TMP/needed" "$TMP/coredef" > "$TMP/needed2"
comm -12 "$TMP/needed2" "$TMP/handdef" > "$TMP/impl"
comm -23 "$TMP/needed2" "$TMP/handdef" > "$TMP/rest"
comm -12 "$TMP/rest"   "$TMP/stubs"   > "$TMP/stubbed"
comm -23 "$TMP/rest"   "$TMP/stubs"   > "$TMP/missing"

NEED=$(wc -l < "$TMP/needed")
IMPL=$(wc -l < "$TMP/impl")
CORE=$(wc -l < "$TMP/bycore")
STUB=$(wc -l < "$TMP/stubbed")
MISS=$(wc -l < "$TMP/missing")

# a generated stub must never stand in for DATA (the _ctype class): classify
# each stubbed symbol by its relocation. PLT32 is a call; data relocations
# without any PLT32 mean the objects read the symbol's bytes. (satoru)
objdump -r $DRV_OBJS $CORE_OBJS 2>/dev/null | awk '
  /^[^ \t].*\.o:/ { f = $1; sub(/:$/, "", f); next }
  NF >= 3 && $2 ~ /^R_X86_64_/ {
    s = $3; sub(/[-+]0x[0-9a-f]+$/, "", s)
    print s, $2
  }
' > "$TMP/relmap"
: > "$TMP/stubdata"
while read -r s; do
  [ -z "$s" ] && continue
  types=$(awk -v s="$s" '$1 == s { print $2 }' "$TMP/relmap")
  if ! echo "$types" | grep -q 'R_X86_64_PLT32' &&
     echo "$types" | grep -qE 'R_X86_64_(REX_GOTPCRELX|GOTPCRELX|GOTPCREL|PC32|32S|32|64)'; then
    echo "$s" >> "$TMP/stubdata"
  fi
done < "$TMP/stubbed"
STUBDATA=$(wc -l < "$TMP/stubdata")

echo "======================================================"
echo " KLD symbol census"
echo "======================================================"
echo "  external symbols needed : $NEED"
echo "  IMPLEMENTED (shim, real): $IMPL"
echo "  IMPORTED CORE (upstream): $CORE"
echo "  STUBBED (aborts)        : $STUB   <- phase 2 work queue"
echo "  STUBBED AS DATA (BUG)   : $STUBDATA   <- data symbol behind a function stub"
AUXDIR=$ROOT/kld/lx_emul/kimpl
AUXFN=$(grep -c '^LX_STUB(' "$AUXDIR/lx_aux_fn_stubs.c" 2>/dev/null || echo 0)
AUXDATA=$(grep -cE '^(struct list_head|int acpi_disabled|DEFINE_MUTEX|struct irq_chip|struct flow_dissector|const struct fwnode_operations|const struct kernel_param_ops|const struct kobj_ns_type_operations|struct static_call_key)' "$AUXDIR/lx_aux_providers.c" 2>/dev/null || echo 0)
echo "  address-taken fn stubs  : $AUXFN   <- address stored, prints if called"
echo "  typed data providers    : $AUXDATA (poison 0)"
echo "  MISSING (no provider)   : $MISS   <- will NOT link"
if [ "$NEED" -gt 0 ]; then
  echo "  real coverage           : $(( (IMPL + CORE) * 100 / NEED ))%"
fi
echo

if [ "$STUBDATA" -gt 0 ]; then
  echo "  data symbols stubbed as functions (must become real data):"
  sed 's/^/    /' "$TMP/stubdata"
  echo
fi

if [ "$MISS" -gt 0 ]; then
  echo "--- MISSING (link will fail) ---"
  cat "$TMP/missing"
  echo
fi

# the phase-2 queue, most-referenced first: a symbol pulled from many call sites
# is usually a core primitive and unblocks the most driver code per unit of work.
if [ "$STUB" -gt 0 ]; then
  echo "--- STUBBED, by reference count (implement from the top) ---"
  nm --undefined-only $DRV_OBJS 2>/dev/null | awk '$1=="U"{print $2}' \
    | grep -Fxf "$TMP/stubbed" 2>/dev/null \
    | sort | uniq -c | sort -rn | head -40
fi

# machine-readable for CI
cp "$TMP/missing" "$BUILD/census_missing.txt" 2>/dev/null || true
cp "$TMP/stubbed" "$BUILD/census_stubbed.txt" 2>/dev/null || true
cp "$TMP/impl"    "$BUILD/census_impl.txt"    2>/dev/null || true
cp "$TMP/bycore"  "$BUILD/census_core.txt"    2>/dev/null || true
cp "$TMP/needed"  "$BUILD/census_needed.txt"  2>/dev/null || true

# CI rule from the doc: fail the build when a symbol is MISSING (unprovided),
# not merely when it is hit at runtime. STUBBED is expected - that is the queue.
if [ "${KLD_CENSUS_STRICT:-0}" = "1" ] && [ "$MISS" -gt 0 ]; then
  echo "symcensus: FAIL - $MISS unprovided symbols (KLD_CENSUS_STRICT=1)" >&2
  exit 1
fi
exit 0
