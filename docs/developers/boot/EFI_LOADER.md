# EFI Loader

`src/boot/efi_loader.c` is a standalone UEFI application that provides an alternative boot path for machines that boot EFI first.

## 1. Purpose

GRUB handles BIOS and most EFI boot scenarios. The EFI loader is for cases where a minimal native EFI application is preferred - for example, custom firmware, Secure Boot setups, or environments where GRUB is not installed.

## 2. How it works

1. The EFI application is registered as a boot entry on the ESP (EFI System Partition).
2. On boot, firmware loads `efi_loader.efi` and hands it the EFI system table.
3. The loader locates the kernel image on the same partition.
4. It calls the firmware's GOP (Graphics Output Protocol) to get a framebuffer description.
5. It sets up a Multiboot-compatible info structure so the kernel does not need to know whether it was loaded by GRUB or EFI.
6. It jumps to the kernel's physical entry point.

## 3. Build notes

The EFI loader is compiled as a PE/COFF binary (the format EFI firmware expects), not as an ELF. It uses a separate compilation step in the Makefile. The main kernel image is ELF; this loader is the bridge between the two worlds.

## 4. Limitations

The EFI loader is intentionally minimal. It does not implement ACPI enumeration, memory map refinement, or device tree setup. It relies on the kernel's own hardware detection for those.

## 5. Related files

- `src/boot/kurono_boot.asm` - entry point the loader jumps to
- `src/boot/kurono_linker.ld` - kernel image layout that the loader must respect
- `src/Makefile` - separate build rules for the EFI binary
