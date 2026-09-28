# Linker script

`src/boot/kurono_linker.ld`. Decides how the kernel image is laid out in memory.

Object files go in, one binary comes out, and this script says where each section lands,
what the load address is, and where the entry point is.

## What it actually sets

- `OUTPUT_FORMAT(elf64-x86-64)` and `ENTRY(_start)`, the boot assembly entry
- the base at `. = 0x100000`, so this is a lower-half kernel
- `/DISCARD/` on `*(.note*)`, `*(.comment)` and `*(.eh_frame)`, so the Multiboot header
  ends up as the very first data in the file
- section placement: `.text` with `*(.mboot)` and `*(.multiboot)` first, then `.rodata`,
  `.init_array`, `.data`, `.bss`
- `kernel_start`
- the `.init_array` span, `__init_array_start` to `__init_array_end`, which
  `RunGlobalConstructors()` walks early in boot. Lose it and static-init registrations
  never run, so the subsystem that registered itself looks like it doesn't exist.
- BSS boundaries, so boot asm can zero it without hard-coded addresses

## When it's wrong

- GRUB rejects the image if the Multiboot header isn't in the first 8KB
- boot asm can't find the BSS boundaries if those symbols aren't exported
- C++ global constructors don't run if `.init_array` is missing
- physical/virtual address confusion if the LMA/VMA split is off

## Files

- `src/boot/kurono_boot.asm` uses the symbols this exports
- `src/boot/multiboot_header.S` placement depends on this
- `src/Makefile` passes it with `-T kurono_linker.ld`
