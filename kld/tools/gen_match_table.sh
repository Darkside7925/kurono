#!/bin/bash
# gen_match_table - the kernel's pci match table, built from the DRIVERS.
#
# For every family that has a manifest in kld/families/ AND a built container
# in build/<family>/, run the container on the HOST with --kld-ids. That mode
# runs the family's initcalls with the pci bus disabled and prints the
# pci_driver id tables the driver registered, so the entries are the driver's
# own MODULE_DEVICE_TABLE data, not a copy maintained here.
#
# Outputs (all under build/, never checked in):
#   build/ids/<family>.ids        the raw driver lines, for humans and tools
#   build/kld_match_table.cpp     the generated table src/Makefile embeds
#
# The cpp is only rewritten when its content changes, so a no-op container
# rebuild does not force a kernel relink.
#
# Usage: gen_match_table.sh [build-dir]
# end (satoru)
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
KLD=$(cd "$HERE/.." && pwd)
BUILD=${1:-$KLD/build}
FAMDIR=$KLD/families
OUT=$BUILD/kld_match_table.cpp
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$BUILD/ids"
: > "$TMP/families"
: > "$TMP/ids"

total=0
nfam=0

for mf in "$FAMDIR"/*.manifest; do
    [ -f "$mf" ] || continue
    fam=$(awk '$1 == "name" { print $2; exit }' "$mf")
    need=$(awk '$1 == "needs_msix_table_page" { print $2; exit }' "$mf")
    lirq=$(awk '$1 == "line_irq" { print $2; exit }' "$mf")
    plat=$(awk '$1 == "platform_mmio" { print $2; exit }' "$mf")
    [ -n "$lirq" ] || lirq=none
    [ -n "$plat" ] || plat=none
    vga=$(awk '$1 == "legacy_vga" { print $2; exit }' "$mf")
    disp=$(awk '$1 == "display" { print $2; exit }' "$mf")
    # d4 properties (all optional, default 0). bar_window lines:
    # "bar_window <bar> <lo>-<hi> ..." (hex offsets, inclusive). (satoru)
    oprg=$(awk '$1 == "opregion" { print $2; exit }' "$mf")
    barp=$(awk '$1 == "bar_policy" { print $2; exit }' "$mf")
    hbr=$(awk '$1 == "host_bridge_read" { print $2; exit }' "$mf")
    win=$(awk '$1 == "bar_window" {
            s = $2 ":"
            for (i = 3; i <= NF; i++) s = s (i > 3 ? "," : "") $i
            out = out (out == "" ? "" : ";") s
        } END { print out }' "$mf")
    case "$win" in *[!0-9a-fA-F:,\;-]*)
        echo "gen_match_table: bad bar_window in $mf" >&2
        exit 1 ;;
    esac
    case "$fam" in
        ""|*[!a-z0-9_]*)
            echo "gen_match_table: bad or missing name in $mf" >&2
            exit 1
            ;;
    esac
    flags=0
    case "$need" in
        0) ;;
        1) flags=$((flags | 1)) ;;   # KldAuto::FAMILY_NEEDS_MSIX_TABLE_PAGE
        *)
            echo "gen_match_table: bad needs_msix_table_page in $mf" >&2
            exit 1
            ;;
    esac
    case "$lirq" in
        none) ;;
        acpi) flags=$((flags | 8)) ;;   # KldAuto::FAMILY_LINE_IRQ_ACPI
        pci)  flags=$((flags | 16)) ;;   # KldAuto::FAMILY_LINE_IRQ_PCI
        *)
            echo "gen_match_table: bad line_irq in $mf" >&2
            exit 1
            ;;
    esac
    case "$plat" in
        none) ;;
        acpi) flags=$((flags | 32)) ;;   # KldAuto::FAMILY_PLATFORM_MMIO_ACPI
        *)
            echo "gen_match_table: bad platform_mmio in $mf" >&2
            exit 1
            ;;
    esac
    case "$vga" in
        "") ;;
        0) ;;
        1) flags=$((flags | 2)) ;;   # KldAuto::FAMILY_LEGACY_VGA
        *)
            echo "gen_match_table: bad legacy_vga in $mf" >&2
            exit 1
            ;;
    esac
    case "$disp" in
        "") ;;
        0) ;;
        1) flags=$((flags | 4)) ;;   # KldAuto::FAMILY_DISPLAY
        *)
            echo "gen_match_table: bad display in $mf" >&2
            exit 1
            ;;
    esac
    for kv in "opregion:$oprg:64" "bar_policy:$barp:128" "host_bridge_read:$hbr:256"; do
        k=${kv%%:*}; r=${kv#*:}; v=${r%%:*}; bit=${r#*:}
        case "$v" in
            ""|0) ;;
            1) flags=$((flags | bit)) ;;   # KldAuto::FAMILY_OPREGION / BAR_POLICY / HOST_BRIDGE_READ
            *) echo "gen_match_table: bad $k in $mf" >&2; exit 1 ;;
        esac
    done
    elf=$BUILD/$fam/kld-$fam.elf
    if [ ! -x "$elf" ]; then
        echo "gen_match_table: $fam not built ($elf absent), skipped" >&2
        continue
    fi
    ids=$BUILD/ids/$fam.ids
    if ! "$elf" --kld-ids > "$TMP/$fam.ids" 2> "$TMP/$fam.err"; then
        echo "gen_match_table: $fam --kld-ids FAILED" >&2
        sed -n '1,40p' "$TMP/$fam.err" >&2
        exit 1
    fi
    # the mode must not talk to the broker at all: the counter in lx_broker.c
    # prints 0 when no SYS_KLD_CALL was made. (satoru)
    # the family's work queue may log a firmware line in the middle of the
    # [kld-ids] line when the host can read /lib/firmware/regulatory.db, so
    # match the counter alone. (satoru)
    if ! grep -q 'broker calls=0' "$TMP/$fam.err"; then
        echo "gen_match_table: $fam --kld-ids made broker calls (see below)" >&2
        grep '\[kld-ids\]' "$TMP/$fam.err" >&2
        exit 1
    fi
    # every line is: drv vendor device subvendor subdevice class class_mask
    awk '
        NF >= 7 && $2 ~ /^[0-9a-f]+$/ && $3 ~ /^[0-9a-f]+$/ && $4 ~ /^[0-9a-f]+$/ &&
        $5 ~ /^[0-9a-f]+$/ && $6 ~ /^[0-9a-f]+$/ && $7 ~ /^[0-9a-f]+$/ {
            printf "    {0x%su, 0x%su, 0x%su, 0x%su, 0x%su, 0x%su},\n", $2, $3, $4, $5, $6, $7
        }
    ' "$TMP/$fam.ids" > "$TMP/$fam.ids.cpp"
    n=$(awk 'END { print NR }' "$TMP/$fam.ids.cpp")
    if [ "$n" -eq 0 ]; then
        echo "gen_match_table: $fam printed no id entries" >&2
        exit 1
    fi
    cp "$TMP/$fam.ids" "$ids"
    if [ -n "$win" ]; then
        printf '    {"%s", %su, %du, %du, "%s"},\n' "$fam" "$flags" "$total" "$n" "$win" >> "$TMP/families"
    else
        printf '    {"%s", %su, %du, %du, nullptr},\n' "$fam" "$flags" "$total" "$n" >> "$TMP/families"
    fi
    cat "$TMP/$fam.ids.cpp" >> "$TMP/ids"
    total=$((total + n))
    nfam=$((nfam + 1))
    echo "gen_match_table: $fam $n entries (needs_msix_table_page=$need legacy_vga=${vga:-0} display=${disp:-0} line_irq=$lirq platform_mmio=$plat)"
done

if [ "$nfam" -eq 0 ]; then
    echo "gen_match_table: no built family with a manifest, no table generated" >&2
    exit 0
fi

LAYOUT=$(sed -n 's/.*FLAG_LAYOUT_VERSION = \([0-9][0-9]*\);.*/\1/p' "$(dirname "$0")/../../src/kernel/kld_auto.h")
[ -n "$LAYOUT" ] || { echo "gen_match_table: FLAG_LAYOUT_VERSION not found in kld_auto.h" >&2; exit 1; }
{
    echo "// AUTO-GENERATED by kld/tools/gen_match_table.sh - do not edit."
    echo "// One entry per pci device id the built KLD families declare, printed by"
    echo "// each family's own container with --kld-ids on the host. (satoru)"
    echo '#include "kld_auto.h"'
    echo
    echo "namespace {"
    echo "const KldAuto::MatchFamily kld_auto_families[] = {"
    cat "$TMP/families"
    echo "};"
    echo "const unsigned kld_auto_family_count = ${nfam}u;"
    echo
    echo "const KldAuto::MatchId kld_auto_ids[] = {"
    cat "$TMP/ids"
    echo "};"
    echo "const unsigned kld_auto_id_count = ${total}u;"
    echo "}  // namespace"
    echo
    echo 'extern "C" {'
    echo "const KldAuto::MatchFamily *kld_auto_families_get(void) { return kld_auto_families; }"
    echo "unsigned kld_auto_family_count_get(void) { return kld_auto_family_count; }"
    echo "const KldAuto::MatchId *kld_auto_ids_get(void) { return kld_auto_ids; }"
    echo "unsigned kld_auto_id_count_get(void) { return kld_auto_id_count; }"
    echo "unsigned kld_auto_layout_get(void) { return ${LAYOUT}u; }"
    echo "}"
    echo
    echo "// end (satoru)"
} > "$TMP/kld_match_table.cpp"

if cmp -s "$TMP/kld_match_table.cpp" "$OUT"; then
    echo "gen_match_table: $nfam families, $total ids, table unchanged"
else
    cp "$TMP/kld_match_table.cpp" "$OUT"
    echo "gen_match_table: $nfam families, $total ids -> $OUT"
fi

# end (satoru)
