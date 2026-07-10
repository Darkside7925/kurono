# Boot Assembly

`src/boot/kurono_boot.asm` is the earliest code that runs after the bootloader hands control to the kernel image.

## 1. What it does

The boot assembly file is responsible for the machine state that C++ cannot reliably establish itself. Specifically it:

1. Validates the Multiboot magic and saves the info pointer before `rax`/`rbx` are clobbered.
2. Sets up a minimal stack in a known safe region.
3. Puts the CPU into 64-bit long mode if the bootloader left it in protected mode.
4. Clears the BSS segment.
5. Transfers control to `kernel_main()` in `kurono_kernel.cpp`.

## 2. Why assembly is needed here

The compiler expects things that do not exist yet: a stack, zeroed BSS, 64-bit mode. The boot assembly creates those preconditions so the C++ kernel can start with its usual assumptions satisfied.

## 3. Multiboot2

`src/boot/multiboot_header.S` contains the Multiboot2 header that GRUB reads to confirm the binary is a valid kernel image and to learn what information to pass at boot.

The Multiboot header must be within the first 8 KB of the kernel image. The linker script (`kurono_linker.ld`) places it at the right offset.

## 4. EFI path

`src/boot/efi_loader.c` is an independent EFI application that loads the kernel image from a FAT partition and jumps to it. It is compiled separately from the main kernel and placed on the ESP. On machines that prefer EFI boot over GRUB, this path takes over.

## 5. Related files

- `src/boot/kurono_linker.ld` - shapes the kernel image layout
- `src/boot/multiboot_header.S` - Multiboot2 header
- `src/boot/efi_loader.c` - EFI entry path
- `src/kernel/kurono_kernel.cpp` - `kernel_main()` that boot assembly calls
