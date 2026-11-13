#!/bin/bash
# end to end regression for the KLD e1000e family: the UNMODIFIED upstream
# e1000e in a ring-3 Kurono container probes QEMU's 82574L, brings the link up,
# completes DHCP and one HTTP GET through Kurono's network stack.
#
#   REGRESS e1000e: PASS
#   REGRESS e1000e: FAIL <reason>
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
SCRATCH="$SCRATCH_ROOT/regress_e1000e_$STAMP"
LOG="$SCRATCH/serial.log"
ISO="$SCRATCH/kurono_e1000e_$STAMP.iso"
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
    echo "REGRESS e1000e: FAIL $1"
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
EXTRA_CMDLINE="kurono.kld=1 kurono.kld.bench=1 kurono.sched=2 kurono.kldbench.port=${HTTP_PORT:-8000}"
if [ "${KLD_INTX:-0}" = 1 ]; then
    # C5.3: force the driver's legacy INTx path, the broker line irq. (satoru)
    EXTRA_CMDLINE="$EXTRA_CMDLINE kurono.kld.nomsi=1"
fi
# KLD_EXTRA adds boot flags (for example kurono.forcex2apic=1). (satoru)
EXTRA_CMDLINE="$EXTRA_CMDLINE ${KLD_EXTRA:-}"
make -C "$ROOT/src" iso -j"$(nproc)" KURONO_KERNEL_BASE=0x14000000 \
    KURONO_KLD_EMBED=1 \
    KURONO_EXTRA_CMDLINE="$EXTRA_CMDLINE" \
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
       grep -q '\[kldbench\] ready: first http GET ok' "$LOG"; then
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
        fail "no first http GET in serial log"
    fi
fi

echo "[regress] markers seen, stopping qemu" >&2
kill "$QP" 2>/dev/null
for _ in $(seq 1 10); do kill -0 "$QP" 2>/dev/null || break; sleep 1; done
kill -9 "$QP" 2>/dev/null
wait "$QP" 2>/dev/null
QP=

echo "[regress] serial log kept at $LOG" >&2

# ── C5.3: the same nic on a legacy line interrupt, plus the guard negatives ──
if [ "${KLD_INTX:-0}" = 1 ]; then
    grep -Eq '\[kldb\] line gsi [0-9]+ isa resolved [0-9]+' "$LOG" \
        || fail "no line attach for the nic (INTx path)"
    grep -Eq '\[kld-ioapic\] program gsi [0-9]+ vector 0x[0-9a-f]+ apic [0-9]+ level active-low masked' "$LOG" \
        || fail "no masked level/active-low redirection entry"
    grep -Eq '\[lx_irq\] line irq [0-9]+ ' "$LOG" \
        || fail "driver did not request the legacy line"
    if grep -q '\[lx_irq\] MSI enabled' "$LOG" || grep -q '\[kldb\] msi ' "$LOG"; then
        fail "an msi path ran under kurono.kld.nomsi=1"
    fi
    if grep -q '\[kldb\] line.*storm' "$LOG"; then
        fail "storm guard tripped on the healthy e1000e line"
    fi
    grep -q '\[kld-ioapic\] lapic svr eoi-suppress=' "$LOG" \
        || fail "no lapic SVR/EOI path line"
    grep -Eq '\[kld-ioapic\] lapic svr eoi-suppress=[01], ioapic version 0x[0-9a-f]+, eoi via (broadcast|eoir)' "$LOG" \
        || fail "EOI path line malformed or refused"

    # the broker contract through the apitest container under the e1000e family:
    # attach/duplicate/pic refusals, the unclaimed storm guard, detach, reattach.
    echo "[regress] building the apitest container for the line negatives" >&2
    make -C "$ROOT/kld" driver=apitest LINUX="$LINUX" -j"$(nproc)" \
        >"$SCRATCH/apitest-build.log" 2>&1 || fail "apitest build (see $SCRATCH/apitest-build.log)"
    cp "$ROOT/kld/build/apitest/kld-apitest.elf" "$ROOT/src/userprogs/kld_apitest.elf" \
        || fail "copy apitest container"
    "$ROOT/kld/tools/fetch_firmware.sh" >"$SCRATCH/fetch.log" 2>&1 \
        || fail "fetch_firmware (see $SCRATCH/fetch.log)"
    AISO="$SCRATCH/kurono_line_$STAMP.iso"
    ALOG="$SCRATCH/apitest.log"
    echo "[regress] building the line-negotiation ISO" >&2
    make -C "$ROOT/src" iso -j"$(nproc)" KURONO_KERNEL_BASE=0x14000000 \
        KURONO_KLD_EMBED=1 KURONO_KLD_FW=1 \
        KURONO_EXTRA_CMDLINE='kurono.kld=apitest kurono.kld.family=e1000e kurono.sched=2 kurono.kld.ioapic.audit=1' \
        >"$SCRATCH/apitest-iso.log" 2>&1 || fail "apitest iso build (see $SCRATCH/apitest-iso.log)"
    cp "$ROOT/build/kurono.iso" "$AISO" || fail "copy apitest iso"
    cp "$ISO" "$SCRATCH/kurono_intx_$STAMP.iso"
    qemu-system-x86_64 -enable-kvm -cpu host -smp "$GUEST_SMP" -m "${GUEST_RAM_MB}M" \
        -machine pc -nodefaults -device VGA,addr=0x3 -boot d -cdrom "$AISO" \
        -display none -no-reboot -serial file:"$ALOG" \
        >"$SCRATCH/apitest-qemu.err" 2>&1 &
    QP=$!
    ok=0
    deadline=$((SECONDS + TIMEOUT))
    while [ "$SECONDS" -lt "$deadline" ]; do
        if [ -f "$ALOG" ] &&
           grep -q '\[apitest\] [0-9]*/[0-9]* PASS' "$ALOG"; then
            ok=1
            break
        fi
        if [ -f "$ALOG" ] && grep -q '\[apitest\] container exited' "$ALOG"; then
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
    [ "$ok" = 1 ] || fail "line negotiation container did not reach N/N PASS (see $ALOG)"
    grep -q 'line/attach_gsi11: PASS' "$ALOG" || fail "line attach negative boot: attach did not pass"
    grep -q 'line/duplicate_refused: PASS' "$ALOG" || fail "line attach negative boot: duplicate not refused"
    grep -q 'line/storm_guard: PASS' "$ALOG" || fail "line attach negative boot: storm guard did not trip"
    grep -q 'line/storm_latched: PASS' "$ALOG" || fail "line attach negative boot: storm guard not latched"
    grep -q 'line/detach: PASS' "$ALOG" || fail "line attach negative boot: detach failed"
    grep -q 'line/reattach_after_detach: PASS' "$ALOG" || fail "line attach negative boot: reattach failed"
    grep -q '\[kld-ioapic\] entry dump: [0-9]* entries, others unchanged: PASS' "$ALOG" \
        || fail "redirection entry dump did not PASS (another entry changed)"
    if grep -q 'line/family_gate' "$ALOG"; then
        fail "line family override did not take (attach was refused)"
    fi

    # inert by default: no kld container, only the stats flag. the i/o apic must
    # never be touched (accesses=0) and no entry may be programmed.
    SISO="$SCRATCH/kurono_inert_$STAMP.iso"
    SLOG="$SCRATCH/inert.log"
    echo "[regress] building the default-boot inert ISO" >&2
    make -C "$ROOT/src" iso -j"$(nproc)" KURONO_KERNEL_BASE=0x14000000 \
        KURONO_EXTRA_CMDLINE='kurono.kld.stats=1' \
        >"$SCRATCH/inert-iso.log" 2>&1 || fail "inert iso build (see $SCRATCH/inert-iso.log)"
    cp "$ROOT/build/kurono.iso" "$SISO" || fail "copy inert iso"
    qemu-system-x86_64 -enable-kvm -cpu host -smp "$GUEST_SMP" -m "${GUEST_RAM_MB}M" \
        -machine pc -nodefaults -device VGA,addr=0x3 -boot d -cdrom "$SISO" \
        -display none -no-reboot -serial file:"$SLOG" \
        >"$SCRATCH/inert-qemu.err" 2>&1 &
    QP=$!
    ok=0
    deadline=$((SECONDS + 180))
    while [ "$SECONDS" -lt "$deadline" ]; do
        if [ -f "$SLOG" ] && grep -q '\[kld-ioapic\] stats: accesses=' "$SLOG"; then
            ok=1
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
    [ "$ok" = 1 ] || fail "default boot never printed the ioapic stats line (see $SLOG)"
    grep -q '\[kld-ioapic\] stats: accesses=0 ' "$SLOG" \
        || fail "default boot touched the I/O APIC (see $SLOG)"
    if grep -q '\[kld-ioapic\] program gsi' "$SLOG"; then
        fail "default boot programmed a redirection entry"
    fi
fi

check_kld_warns
echo "REGRESS e1000e: PASS"
exit 0

# end (satoru)
