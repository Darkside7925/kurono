#!/bin/bash
# end to end regression for the KLD e1000e family: the UNMODIFIED upstream
# e1000e in a ring-3 Kurono container probes QEMU's 82574L, brings the link up,
# completes DHCP and one HTTP GET through Kurono's network stack.
#
#   REGRESS wifi_baseline: PASS
#   REGRESS wifi_baseline: FAIL <reason>
#
# exit 0 on PASS, 1 on FAIL. diagnostics go to stderr; stdout is the one line.
#
# env: LINUX (configured 6.12.99 tree), KLD_SCRATCH (scratch root),
#      KLD_REG_TIMEOUT (seconds, default 240), KLD_SMP (default 4),
#      KLD_MEM_MB (default 2048)
#
# QEMU slot rule for KLD: the guest is -smp 2 -m 2G and we start when
# MemAvailable >= guest RAM + 6 GiB, regardless of other QEMUs. every
# ISO, log and pid file gets a unique name under scratch/kld; we kill only the
# pid we started. if host port 8000 is busy we wait for that port only. (satoru)
# end (satoru)
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
KVER=$(cat "$ROOT/kld/KLD_KERNEL_VERSION")
SCRATCH_ROOT=${KLD_SCRATCH:-/mnt/windows/Users/ibrah/kw/scratch/kld}
STAMP=$(date -u +%Y%m%d_%H%M%S)_$$
SCRATCH="$SCRATCH_ROOT/regress_wifi_baseline_$STAMP"
LOG="$SCRATCH/serial.log"
ISO="$SCRATCH/kurono_wifi_baseline_$STAMP.iso"
TIMEOUT=${KLD_REG_TIMEOUT:-240}
HTTP_PORT=${KLD_HTTP_PORT:-8000}
# 4 vCPUs is the accepted slot; 2 vCPUs is the known starved diagnostic. (satoru)
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
    wait 2>/dev/null
    rm -f "$ISO"
}

fail() {
    echo "REGRESS wifi_baseline: FAIL $1"
    if [ -f "$LOG" ]; then
        { echo "--- serial.log (last 25 lines) ---"; tail -25 "$LOG"; } >&2
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

mkdir -p "$SCRATCH/http"
printf 'hello from kurono kld regression\n' > "$SCRATCH/http/hello.txt"

echo "[regress] building the e1000e container" >&2
make -C "$ROOT/kld" driver=e1000e LINUX="$LINUX" -j"$(nproc)" \
    >"$SCRATCH/build.log" 2>&1 || fail "container build (see $SCRATCH/build.log)"
cp "$ROOT/kld/build/e1000e/kld-e1000e.elf" "$ROOT/src/userprogs/kld_e1000e.elf" \
    || fail "copy container"
cp "$ROOT/kld/build/e1000e/kld-e1000e.elf" "$SCRATCH/kld-e1000e.elf" || fail "keep container ELF"

echo "[regress] building the ISO" >&2
make -C "$ROOT/src" iso -j"$(nproc)" KURONO_KERNEL_BASE=0x14000000 \
    KURONO_KLD_EMBED=1 \
    KURONO_EXTRA_CMDLINE="kurono.kld=1 kurono.kld.bench=1 kurono.kldbench.tls=1 kurono.kldbench.port=$HTTP_PORT kurono.sched=2" \
    >"$SCRATCH/iso.log" 2>&1 || fail "iso build (see $SCRATCH/iso.log)"
[ -f "$ROOT/build/kurono.iso" ] || fail "iso missing after build"
cp "$ROOT/build/kurono.iso" "$ISO" || fail "copy iso"

# QEMU slot: guest RAM + 6 GiB available is enough; other QEMUs may run. (satoru)
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

# the bench hardcodes port 8000: wait for that port, not for zero QEMUs. (satoru)
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

echo "[regress] booting qemu (-smp $GUEST_SMP -m ${GUEST_RAM_MB}M), log $LOG" >&2
qemu-system-x86_64 -enable-kvm -cpu host -smp "$GUEST_SMP" -m "${GUEST_RAM_MB}M" -machine pc -nodefaults \
    -device VGA,addr=0x3 -boot d -cdrom "$ISO" \
    -netdev user,id=n0 -device e1000e,netdev=n0 \
    -display none -no-reboot -serial file:"$LOG" \
    >"$SCRATCH/qemu.err" 2>&1 &
QP=$!

ok=0
deadline=$((SECONDS + TIMEOUT))
while [ "$SECONDS" -lt "$deadline" ]; do
    if [ -f "$LOG" ] &&
       grep -q '\[dhcp\] lease bound' "$LOG" &&
       grep -q '\[kldbench\] https big' "$LOG"; then
        ok=1
        break
    fi
    kill -0 "$QP" 2>/dev/null || break
    sleep 1
done

if [ "$ok" != 1 ]; then
    if [ ! -f "$LOG" ]; then
        fail "qemu produced no serial log"
    elif ! grep -q '\[dhcp\] lease bound' "$LOG"; then
        fail "no dhcp lease in serial log"
    else
        fail "no https big line in serial log"
    fi
fi

echo "[regress] markers seen, stopping qemu" >&2
kill "$QP" 2>/dev/null
for _ in $(seq 1 10); do kill -0 "$QP" 2>/dev/null || break; sleep 1; done
kill -9 "$QP" 2>/dev/null
wait "$QP" 2>/dev/null
QP=

echo "[baseline] serial log kept at $LOG" >&2
check_kld_warns
echo "REGRESS wifi_baseline: PASS"
exit 0

# end (satoru)
