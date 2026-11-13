#!/bin/bash
# end to end regression for the KLD drmcirrus family: the UNMODIFIED upstream
# drm core + tiny/cirrus driver in a ring-3 Kurono container, granted QEMU's
# cirrus-vga as a SECOND display by kurono.kld=auto/take. a drm client commits
# a dumb framebuffer, the host takes a screendump of the cirrus device and
# compares every pixel with the same pattern, then 120 damage flushes run.
#
#   REGRESS drm_cirrus: PASS
#   REGRESS drm_cirrus: FAIL <reason>
#
# env: LINUX (configured 6.12.99 tree), KLD_SCRATCH, KLD_REG_TIMEOUT (240),
#      KLD_SMP (4), KLD_MEM_MB (2048). no network, no host port.
# end (satoru)
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
KVER=$(cat "$ROOT/kld/KLD_KERNEL_VERSION")
SCRATCH_ROOT=${KLD_SCRATCH:-/mnt/windows/Users/ibrah/kw/scratch/kld}
STAMP=$(date -u +%Y%m%d_%H%M%S)_$$
SCRATCH="$SCRATCH_ROOT/regress_drm_cirrus_$STAMP"
LOG="$SCRATCH/serial.log"
ISO="$SCRATCH/kurono_drm_$STAMP.iso"
SHOT="$SCRATCH/shot.ppm"
MON="$SCRATCH/mon.sock"
TIMEOUT=${KLD_REG_TIMEOUT:-240}
GUEST_SMP=${KLD_SMP:-4}
GUEST_RAM_MB=${KLD_MEM_MB:-2048}
GUEST_RAM_KB=$((GUEST_RAM_MB * 1024))
NEED_MEM_KB=$((GUEST_RAM_KB + 6 * 1024 * 1024))
QP=

cleanup() {
    if [ -n "$QP" ] && kill -0 "$QP" 2>/dev/null; then
        kill "$QP" 2>/dev/null
        sleep 1
        kill -9 "$QP" 2>/dev/null
    fi
    wait 2>/dev/null
    rm -f "$ISO"
}

fail() {
    echo "REGRESS drm_cirrus: FAIL $1"
    if [ -f "$LOG" ]; then
        { echo "--- serial.log (last 30 lines) ---"; tail -30 "$LOG"; } >&2
    fi
    exit 1
}

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

mkdir -p "$SCRATCH"
echo "[regress] building the drmcirrus container" >&2
make -C "$ROOT/kld" driver=drmcirrus LINUX="$LINUX" -j"$(nproc)" \
    >"$SCRATCH/build.log" 2>&1 || fail "container build (see $SCRATCH/build.log)"
cp "$ROOT/kld/build/drmcirrus/kld-drmcirrus.elf" "$ROOT/src/userprogs/kld_drmcirrus.elf" \
    || fail "copy container"

echo "[regress] building the ISO" >&2
make -C "$ROOT/src" iso -j"$(nproc)" KURONO_KERNEL_BASE=0x14000000 \
    KURONO_KLD_EMBED=1 \
    KURONO_EXTRA_CMDLINE='kurono.kld=auto kurono.kld.take=1013:00b8' \
    >"$SCRATCH/iso.log" 2>&1 || fail "iso build (see $SCRATCH/iso.log)"
[ -f "$ROOT/build/kurono.iso" ] || fail "iso missing after build"
cp "$ROOT/build/kurono.iso" "$ISO" || fail "copy iso"

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

echo "[regress] booting qemu (-smp $GUEST_SMP -m ${GUEST_RAM_MB}M), log $LOG" >&2
qemu-system-x86_64 -enable-kvm -cpu host -smp "$GUEST_SMP" -m "${GUEST_RAM_MB}M" \
    -machine pc -nodefaults -device VGA,addr=0x3 \
    -device cirrus-vga,id=cirrus0,addr=0x4 \
    -boot d -cdrom "$ISO" -display none -no-reboot -serial file:"$LOG" \
    -monitor unix:"$MON",server,nowait >"$SCRATCH/qemu.err" 2>&1 &
QP=$!

# wait for the client's pattern, then dump the cirrus device before the hold ends.
ok=0
deadline=$((SECONDS + TIMEOUT))
while [ "$SECONDS" -lt "$deadline" ]; do
    if [ -f "$LOG" ] && grep -aq '\[kdrm\] pattern ready' "$LOG"; then ok=1; break; fi
    kill -0 "$QP" 2>/dev/null || break
    sleep 1
done
[ "$ok" = 1 ] || fail "no [kdrm] pattern ready in serial log"

sleep 1
python3 - "$MON" "$SHOT" <<'PY' >"$SCRATCH/screendump.log" 2>&1
import socket, sys, time
sock, shot = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sock)
s.settimeout(2)
try:
    s.recv(65536)
except Exception:
    pass
s.sendall(("screendump %s cirrus0\n" % shot).encode())
time.sleep(1.2)
try:
    while True:
        s.recv(65536)
except Exception:
    pass
s.close()
PY
[ -s "$SHOT" ] || fail "cirrus screendump missing (see $SCRATCH/screendump.log)"

python3 "$ROOT/kld/tests/compare_drm_pattern.py" "$SHOT" >"$SCRATCH/compare.log" 2>&1 \
    || fail "pixel compare failed ($(cat "$SCRATCH/compare.log"))"
grep -q 'pixels exact at 1024x768' "$SCRATCH/compare.log" \
    || fail "unexpected pixel compare result: $(cat "$SCRATCH/compare.log")"

ok=0
deadline=$((SECONDS + 60))
while [ "$SECONDS" -lt "$deadline" ]; do
    if [ -f "$LOG" ] && grep -aq '\[kdrm\] moving block frames=120' "$LOG"; then ok=1; break; fi
    kill -0 "$QP" 2>/dev/null || break
    sleep 1
done
[ "$ok" = 1 ] || fail "no 120-frame completion line"

grep -aq '\[kld-stub\]' "$LOG" && fail "[kld-stub] hit"
check_kld_warns

echo "[regress] compare: $(cat "$SCRATCH/compare.log")" >&2
echo "[regress] serial log kept at $LOG" >&2
echo "REGRESS drm_cirrus: PASS"
exit 0

# end (satoru)
