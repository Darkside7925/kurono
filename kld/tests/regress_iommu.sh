#!/bin/bash
# KLD VT-d end to end regression (M7).
#
# Four phases, q35 with QEMU's virtual IOMMU and an emulated e1000e:
#   A) positive: 3 boots of the ring-3 bench with translation on. Needs the
#      domain line, a DHCP lease and the first HTTP GET, and no fault.
#   B) negative with translation on: the container points one RX buffer at a
#      kernel canary; the device write must fault and the canary must stay
#      intact. The same boot proves the teardown check (context blocked,
#      frames and page tables counted).
#   C) the same negative test with kurono.kld.iommu=0: the canary MUST be
#      overwritten, which proves the test can fail.
#   D) the positive bench once with 39-bit second-level tables
#      (kurono.kld.iommuaw=39), so both SAGAW widths run.
#
#   REGRESS iommu: PASS
#   REGRESS iommu: FAIL <phase>: <reason>
#
# Every boot has its own directory, serial log and monitor socket under the
# scratch root; the ISO is deleted when that boot ends. One QEMU at a time.
#
# env: LINUX, KLD_SCRATCH, KLD_REG_TIMEOUT (default 300), KLD_SMP (4),
#      KLD_MEM_MB (2048)
# end (satoru)
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
KVER=$(cat "$ROOT/kld/KLD_KERNEL_VERSION")
SCRATCH_ROOT=${KLD_SCRATCH:-/mnt/windows/Users/ibrah/kw/scratch/kld}
STAMP=$(date -u +%Y%m%d_%H%M%S)_$$
SCRATCH="$SCRATCH_ROOT/regress_iommu_$STAMP"
TIMEOUT=${KLD_REG_TIMEOUT:-300}
HTTP_PORT=${KLD_HTTP_PORT:-8000}
GUEST_SMP=${KLD_SMP:-4}
GUEST_RAM_MB=${KLD_MEM_MB:-2048}
GUEST_RAM_KB=$((GUEST_RAM_MB * 1024))
NEED_MEM_KB=$((GUEST_RAM_KB + 6 * 1024 * 1024))

QP=
HTTP_PID=
PHASE=setup
LOG=

cleanup() {
    if [ -n "$QP" ] && kill -0 "$QP" 2>/dev/null; then
        kill "$QP" 2>/dev/null
        sleep 1
        kill -9 "$QP" 2>/dev/null
    fi
    if [ -n "$HTTP_PID" ] && kill -0 "$HTTP_PID" 2>/dev/null; then
        kill "$HTTP_PID" 2>/dev/null
    fi
    rm -f "$SCRATCH"/*.iso "$SCRATCH/native.img"
    wait 2>/dev/null
}

fail() {
    echo "REGRESS iommu: FAIL $PHASE: $1"
    if [ -n "$LOG" ] && [ -f "$LOG" ]; then
        { echo "--- $LOG (last 40 lines) ---"; tail -40 "$LOG"; } >&2
    fi
    exit 1
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

mkdir -p "$SCRATCH/http"
printf 'hello from kurono kld regression\n' > "$SCRATCH/http/hello.txt"
dd if=/dev/zero of="$SCRATCH/native.img" bs=1M count=4 status=none \
    || fail "native test disk"

echo "[regress] building the e1000e container" >&2
make -C "$ROOT/kld" driver=e1000e LINUX="$LINUX" -j"$(nproc)" \
    >"$SCRATCH/build.log" 2>&1 || fail "container build (see $SCRATCH/build.log)"
cp "$ROOT/kld/build/e1000e/kld-e1000e.elf" "$ROOT/src/userprogs/kld_e1000e.elf" \
    || fail "copy container"

wait_memory() {
    waited=0
    while :; do
        avail=$(awk '/^MemAvailable:/ { print $2 }' /proc/meminfo)
        [ -n "$avail" ] && [ "$avail" -ge "$NEED_MEM_KB" ] && break
        waited=$((waited + 10))
        [ "$waited" -ge 1200 ] && fail "MemAvailable ${avail:-?} kB below $NEED_MEM_KB kB after 20 min"
        echo "[regress] waiting for memory: ${avail:-?} kB / $NEED_MEM_KB kB" >&2
        sleep 10
    done
}

start_http() {
    waited=0
    while (exec 3<>/dev/tcp/127.0.0.1/$HTTP_PORT) 2>/dev/null; do
        exec 3>&-
        waited=$((waited + 5))
        [ "$waited" -ge 1200 ] && fail "host port $HTTP_PORT busy after 20 min"
        sleep 5
    done
    ( cd "$SCRATCH/http" && exec python3 -m http.server "$HTTP_PORT" --bind 0.0.0.0 \
        >"$SCRATCH/http.log" 2>&1 ) &
    HTTP_PID=$!
    ready=0
    for _ in $(seq 1 50); do
        if (exec 3<>/dev/tcp/127.0.0.1/$HTTP_PORT) 2>/dev/null; then
            exec 3>&-
            ready=1
            break
        fi
        sleep 0.1
    done
    [ "$ready" = 1 ] || fail "http server did not start"
}

iso=

build_iso() {
    name=$1; cmdline=$2
    iso="$SCRATCH/$name.iso"
    echo "[regress] ISO: $cmdline" >&2
    make -C "$ROOT/src" iso -j"$(nproc)" KURONO_KERNEL_BASE=0x14000000 \
        KURONO_KLD_EMBED=1 KURONO_EXTRA_CMDLINE="$cmdline kurono.kldbench.port=$HTTP_PORT" \
        >"$SCRATCH/iso.log" 2>&1 || fail "iso build (see $SCRATCH/iso.log)"
    [ -f "$ROOT/build/kurono.iso" ] || fail "iso missing after build"
    cp "$ROOT/build/kurono.iso" "$iso" || fail "copy iso"
    wait_memory
}

boot() {
    log=$1
    LOG=$log
    echo "[regress] booting qemu (-smp $GUEST_SMP -m ${GUEST_RAM_MB}M), log $log" >&2
    qemu-system-x86_64 -enable-kvm -cpu host -smp "$GUEST_SMP" -m "${GUEST_RAM_MB}M" \
        -machine q35,kernel-irqchip=split -nodefaults -device VGA,addr=0x3 \
        -device intel-iommu,caching-mode=on -boot d -cdrom "$iso" \
        -drive file="$SCRATCH/native.img",if=none,id=native,format=raw \
        -device nvme,serial=kurononative,drive=native,addr=0x4 \
        -netdev user,id=n0 -device e1000e,netdev=n0,addr=0x1 \
        -display none -no-reboot -serial file:"$log" \
        -monitor unix:"$log.mon",server,nowait >"$log.err" 2>&1 &
    QP=$!
}

stop_qemu() {
    if [ -n "$QP" ]; then
        kill "$QP" 2>/dev/null
        for _ in $(seq 1 10); do kill -0 "$QP" 2>/dev/null || break; sleep 1; done
        kill -9 "$QP" 2>/dev/null
        wait "$QP" 2>/dev/null
        QP=
    fi
}

# wait until every pattern is present; fail on panic/exception or timeout.
wait_log() {
    log=$1; shift
    deadline=$((SECONDS + TIMEOUT))
    while [ "$SECONDS" -lt "$deadline" ]; do
        if [ -f "$log" ]; then
            grep -aq "PANIC\|EXCEPTION\|KeBugCheckEx: entered" "$log" && return 2
            all=1
            for pat in "$@"; do
                grep -aq "$pat" "$log" || { all=0; break; }
            done
            [ "$all" = 1 ] && return 0
        fi
        kill -0 "$QP" 2>/dev/null || return 3
        sleep 1
    done
    return 1
}

require_wait() {
    what=$1; shift
    wait_log "$LOG" "$@"
    rc=$?
    case $rc in
        0) ;;
        2) fail "$what: panic/exception in the guest log" ;;
        3) fail "$what: qemu exited early" ;;
        *) fail "$what: markers not reached in ${TIMEOUT}s" ;;
    esac
}

no_fault() {
    what=$1
    grep -aq "\[kld-iommu\] fault" "$LOG" && fail "$what: unexpected IOMMU fault"
    return 0
}

# ---- phase A: positive bench, 3 boots -------------------------------------
PHASE=A
start_http
for i in 1 2 3; do
    build_iso "pos$i" 'kurono.kld=1 kurono.kld.bench=1 kurono.kld.iommu.native=1 kurono.sched=2'
    boot "$SCRATCH/pos$i.log"
    require_wait "pos$i" \
        '\[kld-iommu\] vt-d on:' \
        '\[kld-iommu\] domain 2 for' \
        '\[kld-iommu\] native NVMe read sector 0 PASS' \
        '\[dhcp\] lease bound' \
        '\[kldbench\] ready: first http GET ok'
    no_fault "pos$i"
    stop_qemu
    rm -f "$iso"
    echo "[regress] pos$i PASS" >&2
done

# ---- phase B: negative with translation on --------------------------------
PHASE=B
build_iso neg 'kurono.kld=auto kurono.kld.take=8086:10d3 kurono.kld.iommutest=1 kurono.kld.iommu.native=1 kurono.nettest=1 kurono.sched=2'
boot "$SCRATCH/neg.log"
require_wait "neg" \
    '\[kld-iommu\] domain 2 for e1000e' \
    '\[kld-iommu\] fault bdf=0:1.0' \
    '\[kld-iommu\] canary at .* INTACT .* PASS' \
    '\[kld-iommu\] teardown bdf=0:1.0 context blocked.* PASS'
fault_addr=$(grep -a '\[kld-iommu\] fault bdf=0:1.0' "$LOG" | tail -1 | sed -n 's/.*addr=0x0x0*\([0-9a-fA-F]*\).*/\1/p')
canary_addr=$(grep -a '\[kld-iommu\] canary at' "$LOG" | tail -1 | sed -n 's/.*canary at 0x0x0*\([0-9a-fA-F]*\).*/\1/p')
[ -n "$fault_addr" ] && [ "$fault_addr" = "$canary_addr" ] || fail "negative: fault address '$fault_addr' != canary '$canary_addr'"
stop_qemu
rm -f "$iso"
echo "[regress] negative on PASS (fault at the canary, canary intact, teardown blocked)" >&2

# ---- phase C: the same test with iommu=0 ----------------------------------
PHASE=C
build_iso negoff 'kurono.kld=auto kurono.kld.take=8086:10d3 kurono.kld.iommutest=1 kurono.kld.iommu=0 kurono.nettest=1 kurono.sched=2'
boot "$SCRATCH/negoff.log"
require_wait "negoff" '\[kld-iommu\] canary at .* OVERWRITTEN .* FAIL'
grep -aq '\[kld-iommu\] fault' "$LOG" && fail "negative off: an IOMMU fault was logged with translation off"
stop_qemu
rm -f "$iso"
echo "[regress] negative off PASS (canary overwritten, test can fail)" >&2

# ---- phase D: 39-bit second-level tables ----------------------------------
PHASE=D
build_iso aw39 'kurono.kld=1 kurono.kld.bench=1 kurono.kld.iommu.native=1 kurono.kld.iommuaw=39 kurono.sched=2'
boot "$SCRATCH/aw39.log"
require_wait "aw39" \
    '\[kld-iommu\] vt-d on:' \
    '\[kld-iommu\] domain 2 for .* aw=39' \
    '\[kld-iommu\] native NVMe read sector 0 PASS' \
    '\[dhcp\] lease bound' \
    '\[kldbench\] ready: first http GET ok'
no_fault "aw39"
stop_qemu
rm -f "$iso"
echo "[regress] aw39 PASS" >&2

echo "[regress] logs kept under $SCRATCH" >&2
echo "REGRESS iommu: PASS"
exit 0

# end (satoru)
