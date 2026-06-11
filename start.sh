#!/usr/bin/env bash
# ═══════════════════════════════════════════════════════════════════════════
#  Kurono OS  -  Linux launcher (KVM).  Linux equivalent of start.ps1. (satoru)
#  Builds the ISO and boots it in QEMU with a real GTK window.
# ═══════════════════════════════════════════════════════════════════════════
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$ROOT/src"
ISO="$ROOT/build/kurono.iso"
# boot from a native tmpfs copy: ATAPI CD reads of the ~470MB iso off a fuse
# mount (e.g. /mnt/...) fail under KVM ("Could not read from CDROM"). (satoru)
RUN_ISO="/tmp/kurono_run.iso"

# ── defaults ──
BUILD=1 CLEAN=0 DEBUG=0 CLI=0 CLI_POWEROFF=0 CLI_CMD=""
GPU=1 UEFI=0 HEADLESS=0 RAW_MOUSE=0 NO_USB=0 LOG_STDIO=0 MEM="8G"

usage() {
  cat <<EOF
Kurono OS launcher (Linux/KVM)
usage: ./start.sh [options]
  --no-build         skip the build, just boot the existing ISO
  --clean            make clean before building
  --std              plain framebuffer (-vga std) instead of virtio-gpu accel
  --gpu              force virtio-gpu accelerated display (default ON)
  --cli "<cmd>"      build the CLI boot profile and autorun <cmd> (e.g. "curl http://example.com")
  --cli-poweroff     halt after the CLI autorun finishes
  --uefi             boot via OVMF UEFI firmware instead of SeaBIOS
  --debug            start the GDB stub on :1234 and freeze at reset (-s -S)
  --headless         no window; serial on stdio + QMP at /tmp/kurono.qmp
  --log-stdio        stream the kernel serial to this terminal (debug; can cause lag)
  --no-usb           don't attach the xHCI USB tablet (fall back to PS/2 relative; needs click-to-grab)
  --raw-mouse        boot with kurono.mouse.raw=1 + vmport=off (1:1 PS/2; for synthetic input)
  --mem <size>       RAM (default 8G)
  -h, --help         this help
EOF
}

# ── parse args ──
while [ $# -gt 0 ]; do
  case "$1" in
    --no-build) BUILD=0 ;;
    --clean) CLEAN=1 ;;
    --std) GPU=0 ;;
    --gpu) GPU=1 ;;
    --cli) CLI=1; CLI_CMD="${2:-}"; shift ;;
    --cli-poweroff) CLI_POWEROFF=1 ;;
    --uefi) UEFI=1 ;;
    --debug) DEBUG=1 ;;
    --headless) HEADLESS=1 ;;
    --log-stdio) LOG_STDIO=1 ;;
    --no-usb) NO_USB=1 ;;
    --raw-mouse) RAW_MOUSE=1 ;;
    --mem) MEM="${2:?}"; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option: $1"; usage; exit 1 ;;
  esac
  shift
done

echo "  ╔══════════════════════════════════════╗"
echo "  ║          K U R O N O   O S           ║"
echo "  ╚══════════════════════════════════════╝"

# ── build ──
if [ "$BUILD" -eq 1 ]; then
  ( cd "$SRC"
    [ "$CLEAN" -eq 1 ] && { echo "  [*] make clean"; make clean; }
    if [ "$CLI" -eq 1 ]; then
      echo "  [*] building CLI profile..."
      MK=(KURONO_BOOT_PROFILE=cli KURONO_GRUB_TIMEOUT=0)
      [ -n "$CLI_CMD" ] && MK+=("KURONO_CLI_RUN=${CLI_CMD// /+}")
      [ "$CLI_POWEROFF" -eq 1 ] && MK+=(KURONO_CLI_POWEROFF=1)
      make "${MK[@]}" iso
    else
      echo "  [*] building..."
      make iso
    fi ) || { echo "  [!] build failed"; exit 1; }
  echo "  [+] build ok"
fi

[ -f "$ISO" ] || { echo "  [!] ISO not found at $ISO (build first)"; exit 1; }

# inject the raw-mouse cmdline by patching grub.cfg + repacking (no Makefile var). (satoru)
if [ "$RAW_MOUSE" -eq 1 ]; then
  echo "  [*] enabling raw 1:1 mouse (kurono.mouse.raw=1)"
  sed -i 's#kurono.autologin=1#kurono.autologin=1 kurono.mouse.raw=1#' "$ROOT/build/isodir/boot/grub/grub.cfg" 2>/dev/null || true
  grub-mkrescue -o "$ISO" "$ROOT/build/isodir" >/dev/null 2>&1 || true
fi

cp -f "$ISO" "$RUN_ISO"
printf "  [+] ISO: %s MB\n" "$(( $(stat -c%s "$RUN_ISO") / 1048576 ))"

# ── qemu args ──
command -v qemu-system-x86_64 >/dev/null || { echo "  [!] qemu-system-x86_64 not found"; exit 1; }
[ -e /dev/kvm ] || echo "  [!] /dev/kvm missing  -  falling back to slow TCG emulation"

VGA="virtio"; [ "$GPU" -eq 0 ] && VGA="std"
[ "$GPU" -eq 1 ] && echo "  [*] display: virtio-gpu (accelerated)" || echo "  [*] display: std framebuffer"

# pc (i440fx) + vmport=off: kurono's vmware-vmmouse path is broken (it doesn't read
# the host pointer position, so the cursor never moves); vmport=off disables the
# vmware backdoor so the input stack uses the native USB tablet (absolute) /PS2
# instead. (satoru)
MACHINE="pc,vmport=off"

QARGS=(
  # -smp 1: kurono's scheduler is cooperative / single-CPU ("no preemption yet").
  # under KVM with -smp >1 the APs race the scheduler state and the whole thing
  # deadlocks ~8s in (gui heartbeat stops -> FPS 0, frozen cursor). single CPU is
  # the correct config until SMP-safe scheduling lands. (satoru)
  -machine "$MACHINE" -cpu host -smp 1
  -m "$MEM"
  -cdrom "$RUN_ISO"
  -vga "$VGA"
  -device e1000,netdev=net0
  -netdev user,id=net0,hostfwd=tcp::8080-:80
  -no-reboot -no-shutdown
)
[ -e /dev/kvm ] && QARGS+=(-enable-kvm)

# USB: default to an xHCI controller with a tablet+keyboard so kurono's native
# USB HID driver binds them. the tablet is an ABSOLUTE pointer, so the gtk-window
# cursor tracks the host mouse 1:1 with no grab. --no-usb falls back to a plain
# UHCI tablet (kurono ignores it -> PS/2 relative, needs click-to-grab). (satoru)
if [ "$NO_USB" -eq 1 ]; then
  QARGS+=(-usb -device usb-tablet)
  echo "  [*] USB: off (PS/2 relative input; click the window to grab the mouse)"
else
  QARGS+=(-device qemu-xhci,id=xhci
          -device usb-tablet,bus=xhci.0 -device usb-kbd,bus=xhci.0)
  echo "  [*] USB: xHCI tablet+keyboard (native HID, absolute mouse)"
fi

# ── audio: prefer the NATIVE pipewire backend (the host runs pipewire). the pa
# (pulseaudio-compat) shim spams "set_sink_input_volume failed: Invalid argument"
# and adds latency; native pipewire is clean + lower-latency. fall back to pa, then
# silent. (satoru)
PULSE_SOCK="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/pulse/native"
PW_SOCK="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/pipewire-0"
if [ -S "$PW_SOCK" ] && qemu-system-x86_64 -audiodev help 2>/dev/null | grep -q pipewire; then
  QARGS+=(-audiodev "pipewire,id=snd0")
  echo "  [*] audio: PipeWire (native)"
elif [ -S "$PULSE_SOCK" ]; then
  QARGS+=(-audiodev "pa,id=snd0,server=unix:$PULSE_SOCK")
  echo "  [*] audio: PulseAudio ($PULSE_SOCK)"
else
  QARGS+=(-audiodev "none,id=snd0")
  echo "  [*] audio: none (no audio server found)"
fi
# one HDA codec is enough; the guest auto-selects HDA. dropping AC97/sb16 avoids
# extra emulated-device cpu cost on the single core. (satoru)
QARGS+=(-device intel-hda -device hda-duplex,audiodev=snd0)

# ── UEFI (OVMF) ──
if [ "$UEFI" -eq 1 ]; then
  OVMF_CODE="/usr/share/OVMF/OVMF_CODE_4M.fd"; OVMF_VARS_SRC="/usr/share/OVMF/OVMF_VARS_4M.fd"
  OVMF_VARS="$ROOT/build/OVMF_VARS_4M.fd"
  cp -f "$OVMF_VARS_SRC" "$OVMF_VARS" 2>/dev/null || { echo "  [!] OVMF not found (apt install ovmf)"; exit 1; }
  QARGS+=(-drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE"
          -drive "if=pflash,format=raw,file=$OVMF_VARS")
  echo "  [*] UEFI boot via OVMF"
fi

[ "$DEBUG" -eq 1 ] && { QARGS+=(-s -S); echo "  [*] GDB stub on :1234 (frozen at reset)"; }

# ── display / control plane ──
# IMPORTANT: in the gtk window path, send the kernel serial to a LOG FILE, not the
# terminal. with -serial stdio the boot log + per-frame video/debug spam floods the
# terminal emulator, and that constant text re-rendering steals cpu/gpu from qemu on
# a laptop -> laggy desktop + crackly audio. a quiet terminal = smooth. use
# --log-stdio to stream it live for debugging. (satoru)
SERIAL_LOG="/tmp/kurono-serial.log"
if [ "$HEADLESS" -eq 1 ]; then
  QARGS+=(-display none -serial stdio -qmp "unix:/tmp/kurono.qmp,server,nowait")
  echo "  [*] headless: serial on stdio, QMP at /tmp/kurono.qmp"
elif [ "$LOG_STDIO" -eq 1 ]; then
  QARGS+=(-display gtk,gl=on -serial stdio)
  echo "  [*] serial -> this terminal (--log-stdio); may add lag"
else
  QARGS+=(-display gtk,gl=on -serial "file:$SERIAL_LOG")
  echo "  [*] serial -> $SERIAL_LOG  (terminal stays quiet for a smooth desktop)"
fi

echo "  [+] launching..."; echo "  ──────────────────────────────────────"
exec qemu-system-x86_64 "${QARGS[@]}"
# end (satoru)
