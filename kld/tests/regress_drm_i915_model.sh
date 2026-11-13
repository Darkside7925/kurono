#!/bin/bash
# i915 against the register model (d4.1/d4.2): no hardware. boots the
# kld-i915 container on the kurono.kld.model=i915 test device and keeps the
# probe log, the model's write log and the symbolized writer list.
#
#   regress_drm_i915_model.sh [variant] [extra kernel args]
#   variant: edp (default) | tc | none
#   env: KLD_NOBUILD=1 (reuse the container), KLD_DMC=1 (embed
#        i915/mtl_dmc.bin from the host /lib/firmware, never committed),
#        KLD_REG_TIMEOUT (default 180), KLD_MEM_MB (default 4096),
#        KLD_DISPLAY (e.g. kurono.kld.display=mirror; default none: the
#        family runs the translator test pattern and the model crc test)
#
# outputs in $KLD_SCRATCH/regress_i915_model_<stamp>/: serial.log,
# writes.log ([i915m] w lines), writers.txt (return address -> function),
# result.txt (the stop line). prints
#   REGRESS i915_model: STOP <reason>
# the stop reason is the result: this rig measures how far probe gets. (satoru)
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SCRATCH_ROOT=${KLD_SCRATCH:-/mnt/windows/Users/ibrah/kw/scratch/kld}
STAMP=$(date -u +%Y%m%d_%H%M%S)_$$
VARIANT=${1:-edp}
EXTRA=${2:-}
SCRATCH="$SCRATCH_ROOT/regress_i915_model_$STAMP"
LOG="$SCRATCH/serial.log"
ISO="$SCRATCH/kurono_i915m_$STAMP.iso"
MON="$SCRATCH/mon.sock"
TIMEOUT=${KLD_REG_TIMEOUT:-180}
GUEST_RAM_MB=${KLD_MEM_MB:-4096}
NEED_MEM_KB=$((GUEST_RAM_MB * 1024 + 6 * 1024 * 1024))
LINUX=${LINUX:-/mnt/windows/Users/ibrah/kw/scratch/kld/linux-6.12.99}
ELF="$ROOT/kld/build/i915/kld-i915.elf"
FWDIR="$ROOT/kld/firmware/i915"
QP=

cleanup() {
    if [ -n "$QP" ] && kill -0 "$QP" 2>/dev/null; then
        kill "$QP" 2>/dev/null; sleep 1; kill -9 "$QP" 2>/dev/null
    fi
    wait 2>/dev/null
    rm -f "$ISO"
    [ "${KLD_DMC:-0}" = 1 ] && rm -rf "$FWDIR"
}
stop() { echo "$1" > "$SCRATCH/result.txt"; echo "REGRESS i915_model: STOP $1"; exit 0; }
trap cleanup EXIT
mkdir -p "$SCRATCH"

if [ "${KLD_NOBUILD:-0}" != 1 ]; then
    echo "[i915m] building the i915 container" >&2
    make -C "$ROOT/kld" driver=i915 LINUX="$LINUX" -j"$(nproc)" >"$SCRATCH/build.log" 2>&1 \
        || stop "container build failed (see $SCRATCH/build.log)"
fi
strip --strip-debug -o "$ROOT/src/userprogs/kld_i915.elf" "$ELF" || stop "strip failed"

FWFLAG=
if [ "${KLD_DMC:-0}" = 1 ]; then
    mkdir -p "$FWDIR"
    cp /lib/firmware/i915/mtl_dmc.bin "$FWDIR/" || stop "no host mtl_dmc.bin"
    sha256sum "$FWDIR/mtl_dmc.bin" > "$SCRATCH/dmc.sha256"
    FWFLAG=KURONO_KLD_FW=1
fi

CMD="kurono.kld=auto kurono.kld.take=8086:7d67@fe:0.0 kurono.kld.model=i915:$VARIANT ${KLD_DISPLAY:-} kurono.ptaudit=1 $EXTRA"
echo "$CMD" > "$SCRATCH/cmdline.txt"
make -C "$ROOT/src" iso -j"$(nproc)" KURONO_KERNEL_BASE=0x14000000 KURONO_KLD_EMBED=1 $FWFLAG \
    KURONO_EXTRA_CMDLINE="$CMD" >"$SCRATCH/iso.log" 2>&1 || stop "iso build failed (see $SCRATCH/iso.log)"
cp "$ROOT/build/kurono.iso" "$ISO" || stop "copy iso"

while :; do
    avail=$(awk '/^MemAvailable:/ { print $2 }' /proc/meminfo)
    [ "$avail" -ge "$NEED_MEM_KB" ] && break
    echo "[i915m] waiting for memory: $avail kB / $NEED_MEM_KB kB" >&2
    sleep 10
done

qemu-system-x86_64 -enable-kvm -cpu host -smp 4 -m "${GUEST_RAM_MB}M" \
    -machine pc -nodefaults -device VGA,addr=0x3 \
    -boot d -cdrom "$ISO" -display none -no-reboot -serial file:"$LOG" \
    -monitor unix:"$MON",server,nowait >"$SCRATCH/qemu.err" 2>&1 &
QP=$!

why="timeout ${TIMEOUT}s"
deadline=$((SECONDS + TIMEOUT))
while [ "$SECONDS" -lt "$deadline" ]; do
    kill -0 "$QP" 2>/dev/null || { why="qemu exited"; break; }
    if [ -f "$LOG" ]; then
        l=$(grep -a -m1 -E 'LX_UNIMPL: |\[kld-stub\]|container exited rc=|\[kld-i915\] no drm device|\[kld-i915\] (crc test pipe|crc test: no active|no output to drive)|bartest: (inside|FAIL)|write to broker mapping refused' "$LOG")
        if [ -n "$l" ]; then why="$l"; sleep 8; break; fi
    fi
    sleep 1
done
kill "$QP" 2>/dev/null; sleep 1; kill -9 "$QP" 2>/dev/null; QP=

grep -a '\[i915m\] w ' "$LOG" > "$SCRATCH/writes.log"
awk '{for(i=1;i<=NF;i++) if($i=="w"){print $(i+3); break}}' "$SCRATCH/writes.log" | sort -u > "$SCRATCH/ret.txt"
if [ -s "$SCRATCH/ret.txt" ]; then
    addr2line -f -e "$ELF" $(cat "$SCRATCH/ret.txt") | paste - - | paste "$SCRATCH/ret.txt" - > "$SCRATCH/writers.txt"
fi
echo "[i915m] writes: $(wc -l < "$SCRATCH/writes.log") (outside display: $(grep -c ' X$' "$SCRATCH/writes.log")), log $LOG" >&2
stop "$(echo "$why" | tr -d '\r' | cut -c1-200)"
# end (satoru)
