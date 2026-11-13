#!/bin/bash
# KLD firmware store regression (M8): no firmware in the kernel image, the
# container gets it from a small KURONO FAT32 disk on demand.
#
# Three boots of the apitest family, each with its own 48 MiB GPT+FAT32 disk
# carrying the store built by kld/tools/pack_firmware.sh:
#   - the pnvm is served from the store and the apitest firmware test passes
#     (size, first/last bytes, sha256 checked on the host against MANIFEST.TXT)
#   - a missing name returns -ENOENT and logs "[kld-fw] <family>/<name>: not in store"
#   - "../../../etc/passwd" is refused and logged as a plain-name violation
#
#   REGRESS fwstore: PASS
#   REGRESS fwstore: FAIL <reason>
#
# env: LINUX, KLD_SCRATCH, KLD_REG_TIMEOUT (default 300), KLD_SMP (4),
#      KLD_MEM_MB (2048)
# end (satoru)
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
KVER=$(cat "$ROOT/kld/KLD_KERNEL_VERSION")
SCRATCH_ROOT=${KLD_SCRATCH:-/mnt/windows/Users/ibrah/kw/scratch/kld}
STAMP=$(date -u +%Y%m%d_%H%M%S)_$$
SCRATCH="$SCRATCH_ROOT/regress_fwstore_$STAMP"
TIMEOUT=${KLD_REG_TIMEOUT:-300}
GUEST_SMP=${KLD_SMP:-4}
GUEST_RAM_MB=${KLD_MEM_MB:-2048}
GUEST_RAM_KB=$((GUEST_RAM_MB * 1024))
NEED_MEM_KB=$((GUEST_RAM_KB + 6 * 1024 * 1024))
DISK_MB=48
PART_MB=44
PART_OFF_BYTES=$((2048 * 512))

QP=
LOG=

cleanup() {
    if [ -n "$QP" ] && kill -0 "$QP" 2>/dev/null; then
        kill "$QP" 2>/dev/null
        sleep 1
        kill -9 "$QP" 2>/dev/null
    fi
    rm -f "$SCRATCH"/*.iso "$SCRATCH"/boot*/*.img
    wait 2>/dev/null
}

fail() {
    echo "REGRESS fwstore: FAIL $1"
    if [ -n "$LOG" ] && [ -f "$LOG" ]; then
        { echo "--- $LOG (last 40 lines) ---"; tail -40 "$LOG"; } >&2
    fi
    exit 1
}

trap cleanup EXIT

for t in sgdisk mkfs.vfat mmd mcopy sha256sum qemu-system-x86_64 musl-gcc; do
    command -v "$t" >/dev/null || fail "$t not found"
done
[ -e /dev/kvm ] || fail "/dev/kvm not available"

if [ -z "${LINUX:-}" ]; then
    for c in "$ROOT/../third_party/linux-$KVER" \
             "$SCRATCH_ROOT/linux-$KVER" \
             "/mnt/windows/Users/ibrah/Downloads/OS/third_party/linux-$KVER"; do
        if [ -d "$c/include/linux" ]; then LINUX="$c"; break; fi
    done
fi
[ -n "${LINUX:-}" ] && [ -d "$LINUX/include/linux" ] || fail "no configured linux-$KVER tree (set LINUX=)"
mkdir -p "$SCRATCH"

echo "[regress] building the apitest container" >&2
make -C "$ROOT/kld" driver=apitest LINUX="$LINUX" -j"$(nproc)" \
    >"$SCRATCH/build.log" 2>&1 || fail "container build (see $SCRATCH/build.log)"
cp "$ROOT/kld/build/apitest/kld-apitest.elf" "$ROOT/src/userprogs/kld_apitest.elf" \
    || fail "copy container"

echo "[regress] building the apitest store" >&2
"$ROOT/kld/tools/pack_firmware.sh" apitest --file iwlwifi-gl-c0-fm-c0.pnvm \
    "$SCRATCH/store" >"$SCRATCH/pack.log" 2>&1 \
    || fail "pack_firmware (see $SCRATCH/pack.log)"
STORE=$SCRATCH/store/firmware
MAN=$STORE/apitest/MANIFEST.TXT
[ -f "$MAN" ] || fail "store manifest missing"
read -r name store83 size sha < "$MAN" || fail "manifest unreadable"
[ "$name" = "iwlwifi-gl-c0-fm-c0.pnvm" ] || fail "manifest name '$name'"
host_sha=$(sha256sum "$STORE/apitest/$store83" | cut -d' ' -f1)
[ "$host_sha" = "$sha" ] || fail "store sha256 mismatch ($host_sha != $sha)"
[ "$(stat -c%s "$STORE/apitest/$store83")" = "$size" ] || fail "store size mismatch"
echo "[regress] store: $name -> $store83 $size bytes sha256 $host_sha" >&2

echo "[regress] building the ISO WITHOUT KURONO_KLD_FW" >&2
make -C "$ROOT/src" iso -j"$(nproc)" KURONO_KERNEL_BASE=0x14000000 \
    KURONO_KLD_EMBED=1 \
    KURONO_EXTRA_CMDLINE='kurono.kld=apitest kurono.diskguard=1 kurono.sched=2' \
    >"$SCRATCH/iso.log" 2>&1 || fail "iso build (see $SCRATCH/iso.log)"
[ -f "$ROOT/build/kurono.iso" ] || fail "iso missing after build"

make_disk() {
    d=$1
    mkdir -p "$d"
    dd if=/dev/zero of="$d/disk.img" bs=1M count=$DISK_MB status=none
    sgdisk -n 1:2048:+${PART_MB}M -t 1:0700 -c 1:KURONO "$d/disk.img" >/dev/null \
        || fail "sgdisk"
    dd if=/dev/zero of="$d/part.img" bs=1M count=$PART_MB status=none
    mkfs.vfat -F 32 -n KURONO "$d/part.img" >/dev/null || fail "mkfs.vfat"
    dd if="$d/part.img" of="$d/disk.img" bs=1M seek=1 conv=notrunc status=none
    rm -f "$d/part.img"
    mmd -i "$d/disk.img@@$PART_OFF_BYTES" ::/firmware || fail "mtools mmd firmware"
    mmd -i "$d/disk.img@@$PART_OFF_BYTES" ::/firmware/apitest || fail "mtools mmd family"
    mcopy -i "$d/disk.img@@$PART_OFF_BYTES" "$STORE/apitest/$store83" ::/firmware/apitest/ \
        || fail "mtools store file"
    mcopy -i "$d/disk.img@@$PART_OFF_BYTES" "$MAN" ::/firmware/apitest/ || fail "mtools manifest"
    mcopy -i "$d/disk.img@@$PART_OFF_BYTES" "$STORE/REGULATO.DB" ::/firmware/ || fail "mtools regdb"
    mcopy -i "$d/disk.img@@$PART_OFF_BYTES" "$STORE/REGULATO.P7S" ::/firmware/ || fail "mtools regdb p7s"
}

wait_memory() {
    waited=0
    while :; do
        avail=$(awk '/^MemAvailable:/ { print $2 }' /proc/meminfo)
        [ -n "$avail" ] && [ "$avail" -ge "$NEED_MEM_KB" ] && break
        waited=$((waited + 10))
        [ "$waited" -ge 1200 ] && fail "MemAvailable ${avail:-?} kB below $NEED_MEM_KB kB after 20 min"
        sleep 10
    done
}

for i in 1 2 3; do
    d=$SCRATCH/boot$i
    iso=$d/kurono.iso
    LOG=$d/serial.log
    make_disk "$d"
    cp "$ROOT/build/kurono.iso" "$iso" || fail "copy iso"
    wait_memory
    echo "[regress] boot $i (-smp $GUEST_SMP -m ${GUEST_RAM_MB}M)" >&2
    qemu-system-x86_64 -enable-kvm -cpu host -smp "$GUEST_SMP" -m "${GUEST_RAM_MB}M" \
        -machine pc -nodefaults -device VGA,addr=0x3 -boot d -cdrom "$iso" \
        -drive file="$d/disk.img",if=none,id=kd,format=raw \
        -device nvme,serial=kuronofw,drive=kd \
        -display none -no-reboot -serial file:"$LOG" >"$d/qemu.err" 2>&1 &
    QP=$!
    ok=0
    deadline=$((SECONDS + TIMEOUT))
    while [ "$SECONDS" -lt "$deadline" ]; do
        if [ -f "$LOG" ]; then
            grep -aq "apitest\] [0-9]*/[0-9]* PASS" "$LOG" && { ok=1; break; }
            grep -aq "apitest\] container exited\|PANIC\|EXCEPTION" "$LOG" && break
        fi
        kill -0 "$QP" 2>/dev/null || break
        sleep 1
    done
    kill "$QP" 2>/dev/null
    for _ in $(seq 1 10); do kill -0 "$QP" 2>/dev/null || break; sleep 1; done
    kill -9 "$QP" 2>/dev/null
    wait "$QP" 2>/dev/null
    QP=
    rm -f "$iso"

    [ "$ok" = 1 ] || fail "boot $i: no [apitest] N/N PASS"
    grep -aq "\[kld-fw\] apitest/iwlwifi-gl-c0-fm-c0.pnvm: served from the KURONO store (295036 bytes)" "$LOG" \
        || fail "boot $i: store hit line missing"
    grep -aq "\[kld-fw\] apitest/kld-no-such-firmware.bin: not in store" "$LOG" \
        || fail "boot $i: store miss line missing"
    grep -aq "\[kld-fw\] reject ../../../etc/passwd: not a plain firmware file name" "$LOG" \
        || fail "boot $i: path-escape refuse line missing"
    grep -aq "\[apitest\] firmware_pnvm_enoent_nowait: PASS .*missing=1 escape=1 nowait=1" "$LOG" \
        || fail "boot $i: firmware test detail missing"
    rm -f "$d/disk.img"
    echo "[regress] boot $i PASS (store served, miss logged, escape refused)" >&2
done

echo "[regress] logs kept under $SCRATCH" >&2
echo "REGRESS fwstore: PASS"
exit 0

# end (satoru)
