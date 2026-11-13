#!/bin/bash
# shim self-test regression: builds the apitest KLD family, embeds it and the
# manifest firmware, boots it with kurono.kld=apitest kurono.sched=2 and
# requires the final [apitest] N/N PASS with N equal to the number of test
# lines actually emitted.
#
#   REGRESS apitest: PASS
#   REGRESS apitest: FAIL <reason>
#
# env: LINUX, KLD_SCRATCH, KLD_REG_TIMEOUT (default 300), KLD_SMP (4),
#      KLD_MEM_MB (2048)
# end (satoru)
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
KVER=$(cat "$ROOT/kld/KLD_KERNEL_VERSION")
SCRATCH_ROOT=${KLD_SCRATCH:-/mnt/windows/Users/ibrah/kw/scratch/kld}
STAMP=$(date -u +%Y%m%d_%H%M%S)_$$
SCRATCH="$SCRATCH_ROOT/regress_apitest_$STAMP"
LOG="$SCRATCH/serial.log"
ISO="$SCRATCH/kurono_apitest_$STAMP.iso"
TIMEOUT=${KLD_REG_TIMEOUT:-300}
# 4 vCPUs is the accepted slot; 2 vCPUs is the known starved diagnostic. (satoru)
GUEST_SMP=${KLD_SMP:-4}
GUEST_RAM_MB=${KLD_MEM_MB:-2048}
GUEST_RAM_KB=$((GUEST_RAM_MB * 1024))
NEED_MEM_KB=$((GUEST_RAM_KB + 6 * 1024 * 1024))
QP=

cleanup() {
    if [ -n "$QP" ] && kill -0 "$QP" 2>/dev/null; then
        kill "$QP" 2>/dev/null
        sleep 1
        kill -9 "$QP" 2>/dev/null
    fi
    wait 2>/dev/null
    rm -f "$ISO"
}

fail() {
    echo "REGRESS apitest: FAIL $1"
    if [ -f "$LOG" ]; then
        { echo "--- serial.log (last 40 lines) ---"; tail -40 "$LOG"; } >&2
    fi
    exit 1
}

# a [kld-warn] line is a FAIL unless it matches warn_allowlist.txt. after a
# merge, a new warn is the most valuable line in the log. (satoru)
check_kld_warns() {
    local allow="$ROOT/kld/tests/warn_allowlist.txt" bad=0 w a key ok
    [ -f "$LOG" ] || return 0
    while IFS= read -r w; do
        ok=0
        if [ -f "$allow" ]; then
            while IFS= read -r a; do
                case "$a" in ""|\#*) continue ;; esac
                key=${a%% *}
                case "$w" in *"$key"*) ok=1; break ;; esac
            done < "$allow"
        fi
        if [ "$ok" = 0 ]; then
            echo "[warn] $w" >&2
            bad=1
        fi
    done < <(grep -a '\[kld-warn\]' "$LOG" 2>/dev/null || true)
    [ "$bad" = 0 ] || fail "unallowlisted [kld-warn] line"
}

trap cleanup EXIT

if [ -z "${LINUX:-}" ]; then
    for c in "$ROOT/../third_party/linux-$KVER" \
             "$SCRATCH_ROOT/linux-$KVER" \
             "/mnt/windows/Users/ibrah/Downloads/OS/third_party/linux-$KVER"; do
        if [ -d "$c/include/linux" ]; then LINUX="$c"; break; fi
    done
fi
[ -n "${LINUX:-}" ] && [ -d "$LINUX/include/linux" ] || fail "no configured linux-$KVER tree (set LINUX=)"
command -v qemu-system-x86_64 >/dev/null || fail "qemu-system-x86_64 not found"
command -v musl-gcc >/dev/null || fail "musl-gcc not found"
[ -e /dev/kvm ] || fail "/dev/kvm not available"

mkdir -p "$SCRATCH"

echo "[regress] fetching and verifying firmware" >&2
"$ROOT/kld/tools/fetch_firmware.sh" >"$SCRATCH/fetch.log" 2>&1 || fail "fetch_firmware (see $SCRATCH/fetch.log)"

echo "[regress] building the apitest container" >&2
make -C "$ROOT/kld" driver=apitest LINUX="$LINUX" -j"$(nproc)" \
    >"$SCRATCH/build.log" 2>&1 || fail "container build (see $SCRATCH/build.log)"
cp "$ROOT/kld/build/apitest/kld-apitest.elf" "$ROOT/src/userprogs/kld_apitest.elf" \
    || fail "copy container"
cp "$ROOT/kld/build/apitest/kld-apitest.elf" "$SCRATCH/kld-apitest.elf" || fail "keep container ELF"

echo "[regress] building the ISO with KURONO_KLD_FW=1" >&2
make -C "$ROOT/src" iso -j"$(nproc)" KURONO_KERNEL_BASE=0x14000000 \
    KURONO_KLD_EMBED=1 KURONO_KLD_FW=1 \
    KURONO_EXTRA_CMDLINE='kurono.kld=apitest kurono.sched=2' \
    >"$SCRATCH/iso.log" 2>&1 || fail "iso build (see $SCRATCH/iso.log)"
[ -f "$ROOT/build/kurono.iso" ] || fail "iso missing after build"
cp "$ROOT/build/kurono.iso" "$ISO" || fail "copy iso"

waited=0
while :; do
    avail=$(awk '/^MemAvailable:/ { print $2 }' /proc/meminfo)
    [ -n "$avail" ] && [ "$avail" -ge "$NEED_MEM_KB" ] && break
    waited=$((waited + 10))
    [ "$waited" -ge 1200 ] && fail "MemAvailable ${avail:-?} kB below $NEED_MEM_KB kB after 20 min"
    echo "[regress] waiting for memory: ${avail:-?} kB / $NEED_MEM_KB kB" >&2
    sleep 10
done

echo "[regress] booting qemu (-smp $GUEST_SMP -m ${GUEST_RAM_MB}M), log $LOG" >&2
qemu-system-x86_64 -enable-kvm -cpu host -smp "$GUEST_SMP" -m "${GUEST_RAM_MB}M" \
    -machine pc -nodefaults -device VGA,addr=0x3 -boot d -cdrom "$ISO" \
    -display none -no-reboot -serial file:"$LOG" \
    >"$SCRATCH/qemu.err" 2>&1 &
QP=$!

ok=0
deadline=$((SECONDS + TIMEOUT))
while [ "$SECONDS" -lt "$deadline" ]; do
    if [ -f "$LOG" ] && grep -q '\[apitest\] [0-9]*/[0-9]* PASS' "$LOG"; then
        ok=1
        break
    fi
    if [ -f "$LOG" ] && grep -q '\[apitest\] container exited' "$LOG"; then
        break
    fi
    kill -0 "$QP" 2>/dev/null || break
    sleep 1
done

kill "$QP" 2>/dev/null
for _ in $(seq 1 10); do kill -0 "$QP" 2>/dev/null || break; sleep 1; done
kill -9 "$QP" 2>/dev/null
wait "$QP" 2>/dev/null
QP=

[ -f "$LOG" ] || fail "qemu produced no serial log"
if ! grep -q '\[apitest\] [0-9]*/[0-9]* PASS' "$LOG"; then
    fail "no final [apitest] N/N PASS"
fi
final=$(grep '\[apitest\] [0-9]*/[0-9]* PASS' "$LOG" | tail -1)
num=$(echo "$final" | sed -E 's/.*\[apitest\] ([0-9]+)\/([0-9]+) PASS/\1 \2/')
pass=$(echo "$num" | cut -d' ' -f1)
total=$(echo "$num" | cut -d' ' -f2)
lines=$(grep -c -E '\[apitest\] .*: (PASS|FAIL)' "$LOG")
[ "$pass" = "$total" ] || fail "final line is $pass/$total"
[ "$total" = "$lines" ] || fail "final denominator $total != emitted test lines $lines"
if grep -q -E '\[apitest\] .*: FAIL' "$LOG"; then
    fail "a test failed: $(grep -E '\[apitest\] .*: FAIL' "$LOG" | head -1)"
fi

echo "[regress] serial log kept at $LOG" >&2
echo "$final" >&2
check_kld_warns
echo "REGRESS apitest: PASS"
exit 0

# end (satoru)
