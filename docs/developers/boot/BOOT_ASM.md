# Boot assembly

`src/boot/kurono_boot.asm`. The first code that runs once the bootloader hands over.

It handles the machine state C++ can't set up for itself:

1. check the Multiboot magic and stash the info pointer before `rax`/`rbx` get clobbered
2. build a minimal stack in a known safe region
3. drop the CPU into 64-bit long mode if the bootloader left it in protected mode
4. clear BSS
5. jump to `kernel_main()` in `kurono_kernel.cpp`

## Why asm

The compiler assumes a stack, zeroed BSS and 64-bit mode. None of that exists yet, so
boot asm creates those preconditions and C++ starts with its usual assumptions intact.

## Multiboot2

`src/boot/multiboot_header.S` holds the Multiboot2 header GRUB reads to know the binary
is a kernel and what to pass at boot. It has to sit inside the first 8KB of the image,
and `kurono_linker.ld` puts it there.

## EFI path

`src/boot/efi_loader.c` is a separate EFI application that loads the kernel off a FAT
partition and jumps in. It builds on its own, separately from the kernel, and lives on
the ESP. On machines that prefer EFI over GRUB, this is the path.

## Files

- `src/boot/kurono_linker.ld` image layout
- `src/boot/multiboot_header.S` the Multiboot2 header
- `src/boot/efi_loader.c` EFI entry path
- `src/kernel/kurono_kernel.cpp` `kernel_main()`, what boot asm calls
