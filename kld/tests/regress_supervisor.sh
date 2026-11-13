#!/bin/bash
# kld supervisor regression (kurono.kld.sup=1): the e1000e container comes up,
# an injected failure takes it down, the supervisor restarts it and the network
# bridge re-attaches. no host http server is needed (link + qemu user net only).
#
#   usage: regress_supervisor.sh segv|hang|nobeat|always|exit0
#   nobeat = hang before the first heartbeat (60 s first-beat bound)
#   always = crash on every start (give-up path), exit0 = clean exit, no restart
#   REGRESS supervisor <mode>: PASS / FAIL <reason>
#
# env: LINUX (configured 6.12.99 tree), KLD_SCRATCH, KLD_REG_TIMEOUT (default
# 240), KLD_SMP (default 4), KLD_MEM_MB (default 2048), KLD_SKIP_BUILD=1 reuses
# the container ELF already in src/userprogs, KLD_ISO=<iso> boots that
# iso instead of building one. (satoru)
set -u

MODE=${1:-segv}
case "$MODE" in segv|hang|nobeat|exit0) INJ=$MODE ;; always) INJ=segv-always ;;
    *) echo "usage: $0 segv|hang|nobeat|always|exit0"; exit 2 ;; esac
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
KVER=$(cat "$ROOT/kld/KLD_KERNEL_VERSION")
SCRATCH_ROOT=${KLD_SCRATCH:-/mnt/windows/Users/ibrah/kw/scratch/recov}
STAMP=$(date -u +%Y%m%d_%H%M%S)_$$
SCRATCH="$SCRATCH_ROOT/regress_sup_${MODE}_$STAMP"
LOG="$SCRATCH/serial.log"
ISO="$SCRATCH/kurono_sup_$STAMP.iso"
TIMEOUT=${KLD_REG_TIMEOUT:-240}
GUEST_SMP=${KLD_SMP:-4}
GUEST_RAM_MB=${KLD_MEM_MB:-2048}
QP=

cleanup() {
    if [ -n "$QP" ] && kill -0 "$QP" 2>/dev/null; then
        kill "$QP" 2>/dev/null; sleep 1; kill -9 "$QP" 2>/dev/null
    fi
    wait 2>/dev/null
    rm -f "$ISO"
}
fail() {
    echo "REGRESS supervisor $MODE: FAIL $1"
    [ -f "$LOG" ] && { echo "--- serial.log (kld lines) ---"; grep -a 'kld-sup\|kldnet\|dhcp\|container exited\|faultinject' "$LOG" | tail -30; } >&2
    exit 1
}
trap cleanup EXIT

if [ -z "${LINUX:-}" ]; then
    for c in "$ROOT/../third_party/linux-$KVER" \
             "/mnt/windows/Users/ibrah/kw/scratch/kld/linux-$KVER" \
             "/mnt/windows/Users/ibrah/Downloads/OS/third_party/linux-$KVER"; do
        if [ -d "$c/include/linux" ]; then LINUX="$c"; break; fi
    done
fi
mkdir -p "$SCRATCH"

if [ "${KLD_SKIP_BUILD:-0}" != 1 ]; then
    [ -n "${LINUX:-}" ] && [ -d "$LINUX/include/linux" ] || fail "no configured linux-$KVER tree (set LINUX=)"
    echo "[regress] building the e1000e container" >&2
    make -C "$ROOT/kld" driver=e1000e LINUX="$LINUX" -j"$(nproc)" \
        >"$SCRATCH/build.log" 2>&1 || fail "container build (see $SCRATCH/build.log)"
    cp "$ROOT/kld/build/e1000e/kld-e1000e.elf" "$ROOT/src/userprogs/kld_e1000e.elf" || fail "copy container"
fi
if [ -n "${KLD_ISO:-}" ]; then
    cp "$KLD_ISO" "$ISO" || fail "copy KLD_ISO"
else
echo "[regress] building the ISO" >&2
make -C "$ROOT/src" iso -j"$(nproc)" KURONO_KERNEL_BASE=0x14000000 KURONO_KLD_EMBED=1 \
    KURONO_EXTRA_CMDLINE="kurono.kld=auto kurono.kld.take=8086:10d3 kurono.sched=2 kurono.kld.sup=1 kurono.faultinject=kld:e1000e:$INJ" \
    >"$SCRATCH/iso.log" 2>&1 || fail "iso build (see $SCRATCH/iso.log)"
cp "$ROOT/build/kurono.iso" "$ISO" || fail "copy iso"
fi

echo "[regress] booting qemu (-smp $GUEST_SMP), log $LOG" >&2
qemu-system-x86_64 -enable-kvm -cpu host -smp "$GUEST_SMP" -m "${GUEST_RAM_MB}M" -machine pc -nodefaults \
    -device VGA,addr=0x3 -boot d -cdrom "$ISO" \
    -netdev user,id=n0 -device e1000e,netdev=n0 \
    -display none -no-reboot -serial file:"$LOG" >"$SCRATCH/qemu.err" 2>&1 &
QP=$!

ok=0
deadline=$((SECONDS + TIMEOUT))
endpat='\[kld-sup\] e1000e recovered'
[ "$MODE" = always ] && endpat='FAILED after 2 restarts'
[ "$MODE" = exit0 ] && endpat='container exited rc=0'
while [ "$SECONDS" -lt "$deadline" ]; do
    if [ -f "$LOG" ] && grep -aq "$endpat" "$LOG"; then ok=1; break; fi
    if [ -f "$LOG" ] && grep -aq 'FAILED after 2 restarts\|KERNEL PANIC\|\[PANIC\]' "$LOG"; then break; fi
    kill -0 "$QP" 2>/dev/null || break
    sleep 1
done
# always / exit0: keep the guest 20 s more, to see it keeps running and that
# nothing restarts. (satoru)
if [ "$ok" = 1 ] && [ "$MODE" != segv ] && [ "$MODE" != hang ] && [ "$MODE" != nobeat ]; then sleep 20; fi
kill "$QP" 2>/dev/null; sleep 1; kill -9 "$QP" 2>/dev/null; wait "$QP" 2>/dev/null; QP=
[ -f "$LOG" ] || fail "no serial log"

cnt() { grep -ac "$1" "$LOG"; }
INJLINE=$INJ; [ "$MODE" = always ] && INJLINE=segv
grep -aq '\[dhcp\] lease bound' "$LOG" || fail "no dhcp lease"
grep -aq "\[kld\] faultinject: $INJLINE now" "$LOG" || fail "injected failure not logged"
grep -aiq 'kernel panic\|\[PANIC\]\|#GP\|#DF\|double fault' "$LOG" && fail "kernel fault/panic line"
[ "$ok" = 1 ] || fail "end marker '$endpat' not seen"
case "$MODE" in
segv|hang|nobeat)
    [ "$(cnt '\[kldnet\] eth0 backend = KLD container')" -ge 2 ] || fail "bridge did not re-attach twice"
    [ "$(cnt '\[kldnet\] carrier ON')" -ge 2 ] || fail "carrier did not come up twice"
    [ "$(cnt '\[dhcp\] lease bound')" -ge 2 ] || fail "no second dhcp lease"
    if [ "$MODE" = hang ]; then grep -aq '\[kld-sup\] e1000e hung (no heartbeat for' "$LOG" || fail "hang not detected"; fi
    if [ "$MODE" = nobeat ]; then grep -aq '\[kld-sup\] e1000e hung (no first heartbeat in' "$LOG" || fail "early hang not detected"; fi
    grep -aq '\[kld-sup\] first heartbeat op received' "$LOG" || fail "no heartbeat op from the supervised shim"
    grep -aq '\[kld-sup\] e1000e stopped (rc=.*), restart 1 of 2 in 1 s' "$LOG" || fail "no restart 1 of 2 line"
    ;;
always)
    grep -aq '\[kld-sup\] e1000e stopped (rc=.*), restart 1 of 2 in 1 s' "$LOG" || fail "no restart 1 of 2 line"
    grep -aq '\[kld-sup\] e1000e stopped (rc=.*), restart 2 of 2 in 4 s' "$LOG" || fail "no restart 2 of 2 line"
    [ "$(cnt '\[kld\] faultinject: segv now')" -ge 3 ] || fail "fewer than 3 crashing starts"
    # the system keeps running: some log line after the give-up line. (satoru)
    awk '/FAILED after 2 restarts/{f=1;next} f&&/^\[[0-9]+\]/{n++} END{exit !(n>0)}' "$LOG" || fail "no log activity after give-up"
    ;;
exit0)
    grep -aq '\[kld-sup\] e1000e stopped' "$LOG" && fail "clean exit was restarted"
    [ "$(cnt '\[kld\] container starting')" -eq 1 ] || fail "container started more than once"
    ;;
esac
echo "[regress] serial log kept at $LOG" >&2
echo "REGRESS supervisor $MODE: PASS"
exit 0

# end (satoru)
