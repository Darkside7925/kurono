#!/bin/bash
# S3c runtime half: start the i2chid family in QEMU with no matching LPSS
# device, and require every ordered init step, no stub trap and a clean exit.
#
#   REGRESS i2chid-none: PASS
#   REGRESS i2chid-none: FAIL <reason>
#
# Reuses the apitest embed slot (the family tag is set with kurono.kld.family so
# the broker knows it is the acpi container); KLD_EXTRA adds boot flags. the ISO
# is deleted pass or fail. (satoru)
# end (satoru)
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
KVER=$(cat "$ROOT/kld/KLD_KERNEL_VERSION")
SCRATCH_ROOT=${KLD_SCRATCH:-/mnt/windows/Users/ibrah/kw/scratch/kld}
STAMP=$(date -u +%Y%m%d_%H%M%S)_$$
SCRATCH="$SCRATCH_ROOT/regress_i2chid_none_$STAMP"
LOG="$SCRATCH/serial.log"
ISO="$SCRATCH/kurono_i2chid_none_$STAMP.iso"
TIMEOUT=${KLD_REG_TIMEOUT:-300}
GUEST_SMP=${KLD_SMP:-4}
GUEST_RAM_MB=${KLD_RAM_MB:-2048}
QP=
cleanup() {
    if [ -n "$QP" ] && kill -0 "$QP" 2>/dev/null; then kill "$QP" 2>/dev/null; sleep 1; kill -9 "$QP" 2>/dev/null; fi
    wait 2>/dev/null
    rm -f "$ISO" "$ROOT/build/kurono.iso"
}
fail() {
    echo "REGRESS i2chid-none: FAIL $1"
    [ -f "$LOG" ] && { echo "--- serial.log (last 60 lines) ---"; tail -60 "$LOG"; } >&2
    exit 1
}
trap cleanup EXIT
if [ -z "${LINUX:-}" ]; then
    for c in "$ROOT/../third_party/linux-$KVER" "$SCRATCH_ROOT/linux-$KVER" \
             "/mnt/windows/Users/ibrah/Downloads/OS/third_party/linux-$KVER"; do
        [ -d "$c/include/linux" ] && LINUX=$c && break
    done
fi
[ -n "${LINUX:-}" ] && [ -d "$LINUX/include/linux" ] || fail "no configured linux-$KVER tree"
command -v qemu-system-x86_64 >/dev/null || fail "qemu-system-x86_64 not found"
command -v musl-gcc >/dev/null || fail "musl-gcc not found"
[ -e /dev/kvm ] || fail "/dev/kvm not available"
mkdir -p "$SCRATCH"
echo "[regress] building i2chid" >&2
make -C "$ROOT/kld" driver=i2chid LINUX="$LINUX" -j"$(nproc)" >"$SCRATCH/build.log" 2>&1 \
    || fail "container build (see $SCRATCH/build.log)"
cp "$ROOT/kld/build/i2chid/kld-i2chid.elf" "$ROOT/src/userprogs/kld_apitest.elf" \
    || fail "copy container"
echo "[regress] building ISO (apitest embed slot)" >&2
make -C "$ROOT/src" iso -j"$(nproc)" KURONO_KERNEL_BASE=0x14000000 KURONO_KLD_EMBED=1 \
    KURONO_EXTRA_CMDLINE="kurono.kld=apitest kurono.kld.family=i2chid kurono.sched=2 ${KLD_EXTRA:-}" >"$SCRATCH/iso.log" 2>&1 \
    || fail "iso build (see $SCRATCH/iso.log)"
[ -f "$ROOT/build/kurono.iso" ] || fail "iso missing"
cp "$ROOT/build/kurono.iso" "$ISO" || fail "copy iso"
echo "[regress] booting qemu (-smp $GUEST_SMP -m ${GUEST_RAM_MB}M), log $LOG" >&2
qemu-system-x86_64 -enable-kvm -cpu host -smp "$GUEST_SMP" -m "${GUEST_RAM_MB}M" \
    -machine pc -nodefaults -device VGA,addr=0x3 -boot d -cdrom "$ISO" \
    -display none -no-reboot -serial file:"$LOG" >"$SCRATCH/qemu.err" 2>&1 &
QP=$!
deadline=$((SECONDS + TIMEOUT))
while [ "$SECONDS" -lt "$deadline" ]; do
    if [ -f "$LOG" ] && grep -q '\[kld\] container done' "$LOG"; then break; fi
    if [ -f "$LOG" ] && grep -qE 'container aborting|LX_UNIMPL|\[kld-stub\]' "$LOG"; then break; fi
    kill -0 "$QP" 2>/dev/null || break
    sleep 1
done
kill "$QP" 2>/dev/null
for _ in $(seq 1 10); do kill -0 "$QP" 2>/dev/null || break; sleep 1; done
kill -9 "$QP" 2>/dev/null
wait "$QP" 2>/dev/null
QP=
[ -f "$LOG" ] || fail "qemu produced no serial log"
grep -qE 'container aborting|LX_UNIMPL|\[kld-stub\]' "$LOG" && fail "stub/abort: $(grep -m1 -E 'container aborting|LX_UNIMPL|\[kld-stub\]' "$LOG")"
grep -q '\[kld\] container done' "$LOG" || fail "no [kld] container done"
grep -q 'container exited rc=0' "$LOG" || fail "container did not exit 0"
for s in i2c-core-base pinctrl-meteorpoint intel-lpss intel-lpss-pci i2c-designware-platdrv i2c-hid-acpi hid-multitouch; do
    grep -q "\[i2chid\] init $s rc=0" "$LOG" || fail "missing init step $s"
done
echo "[regress] serial log kept at $LOG" >&2
echo "REGRESS i2chid-none: PASS"
exit 0

# end (satoru)
