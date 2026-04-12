# Linker Script

`src/boot/kurono_linker.ld` controls how the kernel image is laid out in memory.

## 1. What a linker script does

When the compiler produces object files, the linker combines them into a single binary. The linker script tells the linker exactly where each section of that binary should land in memory, what the load address is, and where the kernel entry point lives.

## 2. What is defined here

The linker script for Kurono defines:

- The entry symbol (`_start` in the boot assembly).
- The kernel base virtual address (usually `0xC0000000` for a higher-half kernel or `0x100000` for a lower-half design  -  check the current file for the actual value).
- Section placement: `.text` (code), `.rodata` (read-only data), `.data` (initialized data), `.bss` (zero-initialized data).
- BSS boundaries so the boot assembly can zero it without hard-coding addresses.
- The Multiboot header section so GRUB can find the magic within the first 8 KB.

## 3. Things that break if the linker script is wrong

- GRUB rejects the image if the Multiboot header is not in the first 8 KB.
- The boot assembly fails to find BSS boundaries if those symbols are not exported.
- C++ global constructors do not run if `.init_array` is not included.
- Physical vs virtual address confusion if the LMA/VMA split is wrong.

## 4. Related files

- `src/boot/kurono_boot.asm`  -  uses the symbols this script exports
- `src/boot/multiboot_header.S`  -  section placement depends on this script
- `src/Makefile`  -  passes the linker script with `-T kurono_linker.ld`
