#!/bin/bash
# end to end regression for KLD auto-match. Two boots:
#
#   A) ONLY kurono.kld=auto plus kurono.kld.take=8086:10d3: the kernel must
#      match QEMU's e1000e by its driver-declared id, take it over the native
#      driver because of the take override, start the container, and the
#      container's eth0 must carry DHCP and one HTTP GET.
#   B) ONLY kurono.kld=auto: 8086:10d3 is an id the native e1000 driver binds,
#      so the decision must be "(native driver keeps it)", no container starts
#      and the native stack gets the DHCP lease.
#
#   REGRESS auto: PASS
#   REGRESS auto: FAIL <reason>
#
# exit 0 on PASS, 1 on FAIL. diagnostics go to stderr; stdout is the one line.
# the boot ISOs are removed on exit (pass or fail); logs are kept.
#
# env: LINUX (configured 6.12.99 tree), KLD_SCRATCH (scratch root),
#      KLD_REG_TIMEOUT (seconds, default 300), KLD_SMP (default 4),
#      KLD_MEM_MB (default 2048)
#
# QEMU slot rule for KLD: guest RAM + 6 GiB available, unique names under the
# scratch root, kill only the pid started here, one qemu at a time. (satoru)
# end (satoru)
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
KVER=$(cat "$ROOT/kld/KLD_KERNEL_VERSION")
SCRATCH_ROOT=${KLD_SCRATCH:-/mnt/windows/Users/ibrah/kw/scratch/kld}
STAMP=$(date -u +%Y%m%d_%H%M%S)_$$
SCRATCH="$SCRATCH_ROOT/regress_auto_$STAMP"
LOG_A="$SCRATCH/serial_take.log"
LOG_B="$SCRATCH/serial_native.log"
ISO_A="$SCRATCH/kurono_auto_take_$STAMP.iso"
ISO_B="$SCRATCH/kurono_auto_native_$STAMP.iso"
TIMEOUT=${KLD_REG_TIMEOUT:-300}
HTTP_PORT=${KLD_HTTP_PORT:-8000}
GUEST_SMP=${KLD_SMP:-4}
GUEST_RAM_MB=${KLD_MEM_MB:-2048}
GUEST_RAM_KB=$((GUEST_RAM_MB * 1024))
NEED_MEM_KB=$((GUEST_RAM_KB + 6 * 1024 * 1024))

QP=
HTTP_PID=

cleanup() {
    if [ -n "$QP" ] && kill -0 "$QP" 2>/dev/null; then
        kill "$QP" 2>/dev/null
        sleep 1
        kill -9 "$QP" 2>/dev/null
    fi
    if [ -n "$HTTP_PID" ] && kill -0 "$HTTP_PID" 2>/dev/null; then
        kill "$HTTP_PID" 2>/dev/null
    fi
    rm -f "$ISO_A" "$ISO_B"
    wait 2>/dev/null
}

fail() {
    echo "REGRESS auto: FAIL $1"
    for f in "$LOG_A" "$LOG_B"; do
        if [ -f "$f" ]; then
            { echo "--- $f (last 30 lines) ---"; tail -30 "$f"; } >&2
        fi
    done
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
        if [ "$waited" -ge 1200 ]; then
            fail "MemAvailable ${avail:-?} kB below $NEED_MEM_KB kB after 20 min"
        fi
        echo "[regress] waiting for memory: ${avail:-?} kB / $NEED_MEM_KB kB" >&2
        sleep 10
    done
}

wait_log() {
    log=$1; shift
    deadline=$((SECONDS + TIMEOUT))
    while [ "$SECONDS" -lt "$deadline" ]; do
        if [ -f "$log" ]; then
            all=1
            for pat in "$@"; do
                grep -q "$pat" "$log" || { all=0; break; }
            done
            [ "$all" = 1 ] && return 0
        fi
        kill -0 "$QP" 2>/dev/null || return 1
        sleep 1
    done
    return 1
}

boot_once() {
    iso=$1; log=$2; cmdline=$3
    echo "[regress] building ISO: $cmdline" >&2
    make -C "$ROOT/src" iso -j"$(nproc)" KURONO_KERNEL_BASE=0x14000000 \
        KURONO_KLD_EMBED=1 KURONO_EXTRA_CMDLINE="$cmdline kurono.kldbench.port=$HTTP_PORT" \
        >"$SCRATCH/iso.log" 2>&1 || fail "iso build (see $SCRATCH/iso.log)"
    [ -f "$ROOT/build/kurono.iso" ] || fail "iso missing after build"
    cp "$ROOT/build/kurono.iso" "$iso" || fail "copy iso"

    wait_memory
    echo "[regress] booting qemu (-smp $GUEST_SMP -m ${GUEST_RAM_MB}M), log $log" >&2
    qemu-system-x86_64 -enable-kvm -cpu host -smp "$GUEST_SMP" -m "${GUEST_RAM_MB}M" \
        -machine pc -nodefaults -device VGA,addr=0x3 -boot d -cdrom "$iso" \
        -netdev user,id=n0 -device e1000e,netdev=n0 \
        -display none -no-reboot -serial file:"$log" \
        >"$SCRATCH/qemu_$(basename "$log" .log).err" 2>&1 &
    QP=$!
}

stop_qemu() {
    kill "$QP" 2>/dev/null
    for _ in $(seq 1 10); do kill -0 "$QP" 2>/dev/null || break; sleep 1; done
    kill -9 "$QP" 2>/dev/null
    wait "$QP" 2>/dev/null
    QP=
}

# ---- boot A: take override, traffic through the container -----------------
if (exec 3<>/dev/tcp/127.0.0.1/$HTTP_PORT) 2>/dev/null; then
    exec 3>&-
    fail "host port $HTTP_PORT busy (set KLD_HTTP_PORT to a free port)"
fi
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

boot_once "$ISO_A" "$LOG_A" 'kurono.kld=auto kurono.kld.take=8086:10d3 kurono.nettest=1 kurono.sched=2'

if ! wait_log "$LOG_A" \
        '\[kld-auto\] native wired: deferred, native wifi: kept' \
        '\[kld-auto\] 8086:10d3 at .* -> e1000e (taken: kurono.kld.take)' \
        '\[dhcp\] lease bound' \
        'hello from kurono kld regression'; then
    if [ ! -f "$LOG_A" ]; then
        fail "boot A produced no serial log"
    elif ! grep -q '\[kld-auto\]' "$LOG_A"; then
        fail "boot A: no [kld-auto] decision line"
    elif ! grep -q '(taken: kurono.kld.take)' "$LOG_A"; then
        fail "boot A: e1000e not taken by the override"
    elif ! grep -q '\[dhcp\] lease bound' "$LOG_A"; then
        fail "boot A: no dhcp lease"
    else
        fail "boot A: no http body"
    fi
fi
stop_qemu
kill "$HTTP_PID" 2>/dev/null
HTTP_PID=
rm -f "$ISO_A"

# ---- boot B: no take, native e1000 must keep the device --------------------
boot_once "$ISO_B" "$LOG_B" 'kurono.kld=auto kurono.sched=2'
if ! wait_log "$LOG_B" \
        '\[kld-auto\] native wired: kept, native wifi: kept' \
        '\[kld-auto\] 8086:10d3 at .* -> (native driver keeps it)' \
        '\[kld-auto\] no container started' \
        '\[dhcp\] lease bound'; then
    if [ ! -f "$LOG_B" ]; then
        fail "boot B produced no serial log"
    elif ! grep -q '(native driver keeps it)' "$LOG_B"; then
        fail "boot B: native driver did not keep 8086:10d3"
    elif ! grep -q '\[kld-auto\] no container started' "$LOG_B"; then
        fail "boot B: a container was started without the override"
    elif ! grep -q '\[dhcp\] lease bound' "$LOG_B"; then
        fail "boot B: no native dhcp lease"
    else
        fail "boot B: unexpected log"
    fi
fi
if grep -q '\[kld-auto\] launching' "$LOG_B"; then
    fail "boot B: container launch line present"
fi
stop_qemu
rm -f "$ISO_B"

echo "[regress] serial logs kept at $LOG_A and $LOG_B" >&2
echo "REGRESS auto: PASS"
exit 0

# end (satoru)
