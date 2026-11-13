#!/bin/bash
# kcrash store regression (kurono.kld.sup=1 kurono.kcrash=1): a gpt disk with
# a fat32 partition NAMED KURONO on nvme. boot 1 crashes the e1000e container
# on every start (give-up path) and must write the records + bump the fail
# counter; the host reads them back. boot 2 (no injection) must print the
# previous record; then the host marks e1000e disabled and boot 3 must not
# start it. file sizes never change and fsck.fat stays clean. (satoru)
#
#   REGRESS kcrash: PASS / FAIL <reason>
#
# env: KLD_SCRATCH, KLD_REG_TIMEOUT (per boot, default 240), KLD_SMP (4),
# the e1000e container must already be in src/userprogs (regress_supervisor.sh
# builds it). (satoru)
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SCRATCH_ROOT=${KLD_SCRATCH:-/mnt/windows/Users/ibrah/kw/scratch/recov}
STAMP=$(date -u +%Y%m%d_%H%M%S)_$$
SCRATCH="$SCRATCH_ROOT/regress_kcrash_$STAMP"
TIMEOUT=${KLD_REG_TIMEOUT:-240}
GUEST_SMP=${KLD_SMP:-4}
DISK="$SCRATCH/disk.img"
PART_OFF=$((2048 * 512))
TOOL="python3 -I $ROOT/kld/tests/kcrash_tool.py $DISK $PART_OFF"
BASE='kurono.kld=auto kurono.kld.take=8086:10d3 kurono.sched=2 kurono.kld.sup=1 kurono.kcrash=1 kurono.diskguard=1'
QP=

cleanup() {
    if [ -n "$QP" ] && kill -0 "$QP" 2>/dev/null; then kill "$QP"; sleep 1; kill -9 "$QP" 2>/dev/null; fi
    wait 2>/dev/null
    rm -f "$SCRATCH"/*.iso "$DISK"
}
fail() { echo "REGRESS kcrash: FAIL $1"; exit 1; }
trap cleanup EXIT
mkdir -p "$SCRATCH"

build_iso() {   # $1 out iso, $2 extra cmdline (satoru)
    make -C "$ROOT/src" iso -j"$(nproc)" KURONO_KERNEL_BASE=0x14000000 KURONO_KLD_EMBED=1 \
        KURONO_EXTRA_CMDLINE="$BASE $2" >"$SCRATCH/iso.log" 2>&1 || fail "iso build"
    cp "$ROOT/build/kurono.iso" "$1" || fail "copy iso"
}
if [ -n "${KLD_ISO_CRASH:-}" ]; then cp "$KLD_ISO_CRASH" "$SCRATCH/crash.iso"; else build_iso "$SCRATCH/crash.iso" 'kurono.faultinject=kld:e1000e:segv-always'; fi
if [ -n "${KLD_ISO_PLAIN:-}" ]; then cp "$KLD_ISO_PLAIN" "$SCRATCH/plain.iso"; else build_iso "$SCRATCH/plain.iso" ''; fi

dd if=/dev/zero of="$DISK" bs=1M count=128 status=none
sgdisk -n 1:2048:+100M -t 1:0700 -c 1:KURONO "$DISK" >/dev/null || fail sgdisk
dd if=/dev/zero of="$SCRATCH/part.img" bs=1M count=100 status=none
mkfs.vfat -F 32 -n KURONO "$SCRATCH/part.img" >/dev/null || fail mkfs.vfat
dd if="$SCRATCH/part.img" of="$DISK" bs=1M seek=1 conv=notrunc status=none
rm -f "$SCRATCH/part.img"
# a preallocated boot log so disklog opens cleanly (no 20 s "no boot log" hold). (satoru)
dd if=/dev/zero of="$SCRATCH/KURONO.LOG" bs=1M count=1 status=none
mcopy -i "$DISK@@$PART_OFF" "$SCRATCH/KURONO.LOG" ::/ || fail "mcopy KURONO.LOG"
rm -f "$SCRATCH/KURONO.LOG"

boot() {   # $1 iso, $2 log, $3 end pattern, $4 extra seconds after it (satoru)
    qemu-system-x86_64 -enable-kvm -cpu host -smp "$GUEST_SMP" -m 2048M -machine pc -nodefaults \
        -device VGA,addr=0x3 -boot d -cdrom "$1" \
        -netdev user,id=n0 -device e1000e,netdev=n0 \
        -drive file="$DISK",if=none,id=kd,format=raw -device nvme,serial=kcrashtest,drive=kd \
        -display none -no-reboot -serial file:"$2" >"$SCRATCH/qemu.err" 2>&1 &
    QP=$!
    local dl=$((SECONDS + TIMEOUT)) ok=0
    while [ "$SECONDS" -lt "$dl" ]; do
        grep -aq "$3" "$2" 2>/dev/null && { ok=1; break; }
        kill -0 "$QP" 2>/dev/null || break
        sleep 1
    done
    [ "$ok" = 1 ] && sleep "$4"
    kill "$QP" 2>/dev/null; sleep 1; kill -9 "$QP" 2>/dev/null; wait "$QP" 2>/dev/null; QP=
    grep -aiq 'kernel panic\|\[PANIC\]\|#GP\|#DF\|double fault' "$2" && fail "kernel fault/panic line in $2"
    [ "$ok" = 1 ] || fail "'$3' not seen in $2"
}
sizes() {
    for f in KCRASH.LOG KDRIVERS.CFG KBOOT.CFG; do echo "$f $($TOOL size $f)"; done
}
fsck_clean() {
    dd if="$DISK" of="$SCRATCH/p.img" bs=1M skip=1 count=100 status=none
    fsck.fat -n "$SCRATCH/p.img" >"$SCRATCH/fsck_$1.txt" 2>&1 || { rm -f "$SCRATCH/p.img"; fail "fsck.fat after $1 (see $SCRATCH/fsck_$1.txt)"; }
    rm -f "$SCRATCH/p.img"
    echo "[regress] fsck.fat clean after $1: $(tail -1 "$SCRATCH/fsck_$1.txt")" >&2
}

echo "[regress] boot 1: crash on every start" >&2
L1=$SCRATCH/boot1.log
boot "$SCRATCH/crash.iso" "$L1" '\[kcrash\] KDRIVERS.CFG written' 3
grep -aq '\[kcrash\] created KCRASH.LOG' "$L1" || fail "KCRASH.LOG not created at boot"
[ "$(grep -ac '\[kcrash\] record seq=[0-9]* written to KCRASH.LOG' "$L1")" -ge 3 ] || fail "fewer than 3 records written"
sizes > "$SCRATCH/sizes1.txt"; cat "$SCRATCH/sizes1.txt" >&2
grep -q '^KCRASH.LOG 1048576$' "$SCRATCH/sizes1.txt" || fail "KCRASH.LOG size"
grep -q '^KDRIVERS.CFG 4096$' "$SCRATCH/sizes1.txt" || fail "KDRIVERS.CFG size"
grep -q '^KBOOT.CFG 512$' "$SCRATCH/sizes1.txt" || fail "KBOOT.CFG size"
$TOOL log > "$SCRATCH/log1.txt"; $TOOL drv > "$SCRATCH/drv1.txt"; $TOOL boot > "$SCRATCH/kboot1.txt"
cat "$SCRATCH/log1.txt" "$SCRATCH/drv1.txt" "$SCRATCH/kboot1.txt" >&2
grep -q 'header magic_ok=1 version=1 next=3 count=3' "$SCRATCH/log1.txt" || fail "KCRASH.LOG header"
grep -q 'seq=1 boot=1 kind=1 family=e1000e bdf=200 id=8086:10d3 pid=[0-9]* rc=\(-3\|139\) sig=[0-9]* .*attempt=1 result=1 .*csum_ok=1' "$SCRATCH/log1.txt" || fail "record 1"
grep -q 'seq=2 .*family=e1000e .*attempt=2 result=1 .*csum_ok=1' "$SCRATCH/log1.txt" || fail "record 2"
grep -q 'seq=3 .*family=e1000e .*attempt=2 result=2 action="gave up after 2 restarts" .*csum_ok=1' "$SCRATCH/log1.txt" || fail "record 3"
grep -q 'drv family=e1000e policy=0 fails=1 last_fail_boot=1' "$SCRATCH/drv1.txt" || fail "fail counter"
grep -q 'kboot magic_csum_ok=1 boot_count=1' "$SCRATCH/kboot1.txt" || fail "boot counter"
fsck_clean boot1

echo "[regress] boot 2: no injection, previous record printed" >&2
L2=$SCRATCH/boot2.log
boot "$SCRATCH/plain.iso" "$L2" '\[kldnet\] carrier ON' 2
grep -aq '\[kcrash\] last: seq=3 boot=1 kind=KLD family=e1000e .*result=gave-up' "$L2" || fail "no '[kcrash] last:' line"
grep -aq '\[kcrash\] KBOOT.CFG boot_count=2' "$L2" || fail "boot counter 2"
sizes > "$SCRATCH/sizes2.txt"; cmp -s "$SCRATCH/sizes1.txt" "$SCRATCH/sizes2.txt" || fail "file sizes changed"
fsck_clean boot2

echo "[regress] boot 3: e1000e disabled from the host" >&2
$TOOL setpolicy e1000e disabled >&2 || fail "setpolicy"
L3=$SCRATCH/boot3.log
boot "$SCRATCH/plain.iso" "$L3" 'e1000e disabled in KDRIVERS.CFG, not started' 8
grep -aq '\[kld\] container starting' "$L3" && fail "disabled family was started"
grep -aq '\[kcrash\] KBOOT.CFG boot_count=3' "$L3" || fail "boot counter 3"
sizes > "$SCRATCH/sizes3.txt"; cmp -s "$SCRATCH/sizes1.txt" "$SCRATCH/sizes3.txt" || fail "file sizes changed (boot 3)"
fsck_clean boot3
echo "[regress] logs kept in $SCRATCH" >&2
echo "REGRESS kcrash: PASS"
exit 0

# end (satoru)
