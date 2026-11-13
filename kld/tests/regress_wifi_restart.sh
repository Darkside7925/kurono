#!/bin/bash
# real-card container kill/restart loop (KLD supervisor on the BE200).
# One ISO, N boots (KLD_RESTART_N, default 5). Every boot: the supervisor
# starts the iwlwifi container on 0000:82:00.0 (kurono.kld=auto pins it to that
# bdf), the first start is killed by fault injection (segv), the supervisor
# releases it and restarts it; the rig waits for "recovered", lets the second
# instance run 15 s, then stops QEMU (SIGTERM first). Per boot it requires: the
# injection, restart 1 of 2, recovered, a VT-d domain released after the kill
# and a second domain created, no [kld-warn] at all (so no "queue full"), no
# fault or DMAR fault line, and it prints the pmm_free_mb of every container
# exit so the baseline can be read. q35 + intel-iommu like the connect gates.
#
#   REGRESS wifi_restart: PASS | FAIL <reason>
# env: LINUX, KLD_SCRATCH, KLD_RESTART_N, KLD_SMP (4), KLD_MEM_MB (2048)
# end (satoru)
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
KVER=$(cat "$ROOT/kld/KLD_KERNEL_VERSION")
SCRATCH_ROOT=${KLD_SCRATCH:-/mnt/windows/Users/ibrah/kw/scratch/kld}
SCRATCH="$SCRATCH_ROOT/regress_wifi_restart_$(date -u +%Y%m%d_%H%M%S)_$$"
BDF=0000:82:00.0
N=${KLD_RESTART_N:-5}
GUEST_SMP=${KLD_SMP:-4}
GUEST_RAM_MB=${KLD_MEM_MB:-2048}
NEED_MEM_KB=$((GUEST_RAM_MB * 1024 + 6 * 1024 * 1024))
ISO="$SCRATCH/restart.iso"
QP=
alive() { [ -n "$QP" ] && sudo -n kill -0 "$QP" 2>/dev/null; }
stopq() { if alive; then sudo -n kill "$QP" 2>/dev/null; sleep 2; alive && { sleep 3; sudo -n kill -9 "$QP" 2>/dev/null; }; fi; QP=; }
cleanup() { stopq; rm -f "$ISO"; }
trap cleanup EXIT
fail() { echo "REGRESS wifi_restart: FAIL $1"; exit 1; }

if [ -z "${LINUX:-}" ]; then LINUX="$SCRATCH_ROOT/linux-$KVER"; fi
[ -d "$LINUX/include/linux" ] || fail "no configured linux tree"
DRV=$(basename "$(readlink -f "/sys/bus/pci/devices/$BDF/driver" 2>/dev/null)" 2>/dev/null)
[ "$DRV" = vfio-pci ] || fail "$BDF is not bound to vfio-pci (driver=${DRV:-none})"
sudo -n true 2>/dev/null || fail "sudo -n not available"
mkdir -p "$SCRATCH"
"$ROOT/kld/tools/fetch_firmware.sh" >"$SCRATCH/fetch.log" 2>&1 || fail "fetch_firmware"
make -C "$ROOT/kld" driver=iwlwifi LINUX="$LINUX" -j"$(nproc)" >"$SCRATCH/build.log" 2>&1 || fail "container build"
cp "$ROOT/kld/build/iwlwifi/kld-iwlwifi.elf" "$ROOT/src/userprogs/kld_iwlwifi.elf" || fail "copy container"
rm -f "$ROOT/build/kurono_base.elf" "$ROOT/build/kurono.elf" "$ROOT/build/kurono.iso"
make -C "$ROOT/src" iso -j"$(nproc)" KURONO_KERNEL_BASE=0x14000000 KURONO_KLD_EMBED=1 KURONO_KLD_FW=1 \
    KURONO_EXTRA_CMDLINE="kurono.kld=auto kurono.kld.take=8086:272b kurono.sched=2 kurono.kld.sup=1 kurono.faultinject=kld:iwlwifi:segv ${KLD_EXTRA_CMDLINE:-}" \
    >"$SCRATCH/iso.log" 2>&1 || fail "iso build"
cp "$ROOT/build/kurono.iso" "$ISO" || fail "copy iso"

for b in $(seq 1 "$N"); do
    LOG="$SCRATCH/boot$b.log"
    waited=0
    while :; do
        avail=$(awk '/^MemAvailable:/ { print $2 }' /proc/meminfo)
        [ "$avail" -ge "$NEED_MEM_KB" ] && break
        waited=$((waited + 10)); [ "$waited" -ge 1200 ] && fail "boot $b: MemAvailable below $NEED_MEM_KB kB"
        sleep 10
    done
    rm -f "$SCRATCH/qemu.pid"
    sudo -n qemu-system-x86_64 -enable-kvm -cpu host -smp "$GUEST_SMP" -m "${GUEST_RAM_MB}M" \
        -machine q35,kernel-irqchip=split -device intel-iommu,caching-mode=on,aw-bits=39 -nodefaults -nic none \
        -device VGA,addr=0x3 -device vfio-pci,host="$BDF" -boot d -cdrom "$ISO" \
        -display none -no-reboot -serial file:"$LOG" -pidfile "$SCRATCH/qemu.pid" \
        >"$SCRATCH/qemu$b.err" 2>&1 &
    for _ in $(seq 1 50); do [ -f "$SCRATCH/qemu.pid" ] && break; sleep 0.2; done
    QP=$(cat "$SCRATCH/qemu.pid" 2>/dev/null)
    deadline=$((SECONDS + 240)); ok=0
    while [ "$SECONDS" -lt "$deadline" ]; do
        if [ -f "$LOG" ] && grep -aq '\[kld-sup\] iwlwifi recovered' "$LOG"; then ok=1; break; fi
        if [ -f "$LOG" ] && grep -aq 'FAILED after 2 restarts\|KERNEL PANIC\|\[PANIC\]' "$LOG"; then break; fi
        alive || break
        sleep 1
    done
    [ "$ok" = 1 ] && sleep 15
    stopq
    sleep 3
    [ -f "$LOG" ] || fail "boot $b: no serial log"
    [ "$ok" = 1 ] || fail "boot $b: no 'iwlwifi recovered' (log $LOG)"
    grep -aq '\[kld\] faultinject: segv now' "$LOG" || fail "boot $b: injection not logged"
    grep -aq '\[kld-sup\] iwlwifi stopped (rc=.*), restart 1 of 2' "$LOG" || fail "boot $b: no restart line"
    rel=$(grep -ac '\[kld-iommu\] domain .* released: device blocked' "$LOG")
    dom=$(grep -ac '\[kld-iommu\] domain .* for iwlwifi bdf ' "$LOG")
    [ "$rel" -ge 1 ] || fail "boot $b: no VT-d domain release after the kill"
    [ "$dom" -ge 2 ] || fail "boot $b: restart did not create a second VT-d domain ($dom)"
    if grep -aq '\[kld-warn\]' "$LOG"; then fail "boot $b: $(grep -a -m1 '\[kld-warn\]' "$LOG" | cut -c1-120)"; fi
    # the injected segv prints exactly one [ffault] block for the container; any other is a fault. (satoru)
    [ "$(grep -ac '\[ffault\] pid=' "$LOG")" -le 1 ] || fail "boot $b: more than the injected fault"
    if grep -aqE 'PANIC|EXCEPTION|\[kld-iommu\] fault|queue full|LX_UNIMPL' "$LOG"; then fail "boot $b: panic/DMAR/queue-full line"; fi
    echo "[restart] boot $b ok: domains=$dom released=$rel $(grep -a -m1 '\[kld-iommu\] domain .* released' "$LOG" | sed 's/.*released/released/')"
    echo "[restart] boot $b pmm_free_mb at container exits: $(grep -a 'sched2.*exited code' "$LOG" | sed -n 's/.*pmm_free_mb=\([0-9]*\).*/\1/p' | tr '\n' ' ')"
done
echo "REGRESS wifi_restart: PASS ($N boots, logs in $SCRATCH)"
exit 0
