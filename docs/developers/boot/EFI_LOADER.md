# EFI loader

`src/boot/efi_loader.c`. Standalone UEFI application, the boot path for machines that
come up EFI first.

GRUB covers BIOS and most EFI cases. This exists for when you want a minimal native EFI
app instead: custom firmware, Secure Boot setups, boxes with no GRUB installed.

## How it runs

1. registered as a boot entry on the ESP
2. firmware loads `efi_loader.efi` and hands over the EFI system table
3. the loader finds the kernel image on the same partition
4. it asks firmware's GOP (Graphics Output Protocol) for a framebuffer description
5. it builds a Multiboot-compatible info structure, so the kernel doesn't care whether
   GRUB or EFI loaded it
6. it jumps to the kernel's physical entry point

## Build

Compiled as PE/COFF, which is what EFI firmware expects, not ELF. Separate Makefile
step. The kernel image is ELF, so this loader is the bridge between the two worlds.

## Limits

Deliberately minimal. No ACPI enumeration, no memory map refinement, no device tree.
The kernel's own hardware detection covers all of that.

## Files

- `src/boot/kurono_boot.asm` where the loader jumps
- `src/boot/kurono_linker.ld` the image layout the loader has to respect
- `src/Makefile` the separate EFI build rules
