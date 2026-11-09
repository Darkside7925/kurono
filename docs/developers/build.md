# Building Kurono

All build commands run from `src/`. Output goes to `build/` at the repo root.

## What you need

- A native `g++` (it falls back to plain `g++ -m64` if there is no `x86_64-elf-*` cross
  toolchain; a cross toolchain is used automatically if you have one)
- NASM
- `musl-gcc` (Debian: `apt install musl-tools`). The ring-3 kui apps, the pthread test and
  other userland programs are built with it. Without it the build still links, but the
  pieces that need it are left out.
- `grub-mkrescue`, `xorriso`, `mtools` (and `grub-pc-bin`, `grub-efi-amd64-bin`) for the ISO
- the standalone EFI loader needs gnu-efi (Debian: `apt install gnu-efi`; see the `EFI_*` variables in
  `src/Makefile`)
- QEMU for testing

## The commands I use

```bash
cd src
make -j$(nproc)                                             # kernel only
make iso -j$(nproc) KURONO_KERNEL_BASE=0x14000000           # full bootable ISO
make gpu-efi                                                # separate GPU test EFI loader
make clean
```

`make iso` builds the kernel, both EFI loaders (normal and emergency), packs everything
and runs `grub-mkrescue`.

### Why `KURONO_KERNEL_BASE=0x14000000`

By default the kernel links at 1 MB. Some big prebuilt Linux binaries are fixed-address
(ET_EXEC) executables that want to live in the low part of the address space, and the
default kernel base collides with them. `KURONO_KERNEL_BASE=0x14000000` links the kernel
above that range. I build with it every time, and every boot-tested build mentioned in
the docs used it.

## Outputs

| file | what |
| --- | --- |
| `build/kurono_base.elf` | kernel the GRUB entries load |
| `build/kurono.elf` | full kernel with the installer payloads embedded |
| `build/kurono.iso` | bootable ISO (BIOS GRUB, UEFI GRUB, and the EFI loaders) |
| `build/boot/kurono_efi.efi` | standalone EFI loader, boots with `kurono.autologin=1` |
| `build/boot/kurono_emergency.efi` | EFI loader, text mode emergency boot |
| `build/boot/kurono_efi_gpu.efi` | `make gpu-efi` only. GPU test loader, never the default |

## Build knobs

Passed as `make VAR=value`:

- `KURONO_KERNEL_BASE` see above
- `KURONO_SLIM=1` leave the big optional blobs out of the kernel image (much faster
  boot, because most of the boot time on slow media is just loading the image)
- `KURONO_GRUB_TIMEOUT` seconds the GRUB menu waits (default 10)
- `KURONO_EXTRA_CMDLINE='...'` extra flags appended to the main GRUB entry. I use this
  for headless self-tests
- `KURONO_CLI_RUN`, `KURONO_GUI_RUN` auto-run a shell command or a GUI app at boot
  (becomes `kurono.cli.run=` / `kurono.gui.run=`)
- `KURONO_AUTOLOGIN=1` bake `kurono.autologin=1` into the main entry

## Embedded binary blobs

Some builds embed binary assets (`src/assets_fonts.tar`, `src/assets_xkb.tar`,
`src/assets_nsslibs.tar`, prebuilt tools in `src/userprogs/`). They're optional and the
build treats missing ones as weak symbols. Without the NSS libs, for example, Firefox
can't do HTTPS.

## Testing in QEMU

```bash
qemu-system-x86_64 -enable-kvm -cpu host -m 4G -smp 4 \
  -cdrom build/kurono.iso -boot d -vga std \
  -device qemu-xhci -device usb-tablet \
  -device e1000,netdev=n0 -netdev user,id=n0 \
  -serial file:serial.log
```

Things I learned the hard way:

- Read the serial log first. A black screen nearly always explains itself there.
- `-boot d` once there's a second disk. Otherwise QEMU boots the blank disk.
- Test input with `usb-kbd` / `usb-tablet`, not PS/2. USB HID is what the real laptop
  uses.
- Only run one QEMU at a time on a small machine. Two at once gave me empty logs.
- `-kernel` can't boot the 64-bit Multiboot2 image directly. Boot the GRUB ISO.
- To swap in a fresh kernel without a full rebuild, repack the ISO with
  `xorriso -indev old.iso -outdev new.iso -boot_image any replay -update ...`.
- Under QEMU without KVM (TCG) everything is slow, Firefox especially.

## Headless self-tests

Many subsystems have a boot-flag self-test (for example `kurono.kmemxtest`,
`kurono.kcltest`, `kurono.klstest`, `kurono.kdf.test`, `kurono.dyntest`,
`kurono.kfstest`, `kurono.wifitest`, `kurono.kentest`). They print results to serial and
many accept a `.poweroff` variant so a script can run them unattended. See `boot-flags.md`.

## Real hardware

Boot through the EFI loader, from a USB stick or the ESP. Read
`../beginners/boot-on-hardware.md` first. The disk guard (read-only except the `KURONO`
partition) is on for EFI boots.
