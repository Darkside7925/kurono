# Try Kurono in a VM

This is the safest way. Nothing touches your real disks.

## What you need

- A Linux machine (Debian-ish is what I use) or WSL.
- `qemu-system-x86_64`.
- The ISO: either build it (see below) or use one I've published.
- KVM helps a lot (`/dev/kvm`). Without it QEMU falls back to software emulation (TCG).
  Kurono runs, but Firefox in particular will crawl.

## Get an ISO

Building from source:

```bash
sudo apt install build-essential nasm musl-tools grub-pc-bin grub-efi-amd64-bin xorriso mtools qemu-system-x86
cd src
make iso -j$(nproc) KURONO_KERNEL_BASE=0x14000000
```

The result is `build/kurono.iso`. It needs `musl-gcc` (from `musl-tools`) because the
ring-3 apps are built with it. More detail in `../developers/build.md`.

## Boot it

The launcher script at the repo root does the sensible thing (KVM, virtio-gpu, audio, a
USB tablet so the mouse behaves, a persistent data disk):

```bash
./start.sh                 # build and boot
./start.sh --no-build      # just boot the existing ISO
./start.sh --mem 4G --smp 4
```

Or by hand:

```bash
qemu-system-x86_64 -enable-kvm -cpu host -m 4G -smp 4 \
  -cdrom build/kurono.iso -boot d \
  -vga std -device qemu-xhci -device usb-tablet \
  -device e1000,netdev=n0 -netdev user,id=n0
```

Notes from experience:

- Use `-boot d` once you attach a second disk, otherwise QEMU tries the blank disk first
  and you get a black screen.
- Use `usb-tablet` for the mouse. PS/2 relative mouse in QEMU is accelerated and feels
  weird.
- Stick with the default `pc` machine. Last time I tried `-M q35` it did not boot.
- Kurono boots with 512 MB RAM. Give it 2 to 4 GB if you want Firefox or Alpine.
- Alpine in the hypervisor needs VT-x. Inside a VM that means nested virtualization has
  to be on for the outer VM.

## The GRUB menu

The ISO has several entries. The ones you care about:

- **Kurono OS (Multiboot2)** the normal one.
- **Kurono OS (EFI Direct)** the EFI loader path, used on UEFI.
- **Kurono Setup** the first-run/setup flow.
- **Emergency** entries, a text-mode fallback if the graphics path fails.

To add a boot flag, press `e` on the entry in GRUB and append it to the `multiboot2` line.
All the flags are in `../developers/boot-flags.md`. Two you probably want:

- `kurono.autologin=1` skip the login screen
- `kurono.sched=2` use every CPU core

## Things to try

- Open the apps from the dock. Calculator, files, editor, terminal, settings.
- Super+V (clipboard history), Alt+Tab, Super+Left/Right (snap).
- Open the control center from the menu bar tray.
- Open the browser. It starts Firefox. The first launch downloads and installs it.
- In the terminal, `help` lists commands. `ken <cmd>` runs something elevated and shows
  the approval prompt.

## If something goes wrong

Add `-serial stdio` (or `-serial file:log.txt`) to QEMU. The kernel logs everything to
the serial port, and a black screen almost always has a useful reason in there.
