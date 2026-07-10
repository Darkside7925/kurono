/* Linker script for Kurono test user program.
   Loads at 0x40100000  -  inside the kernel's USERSPACE_BASE window
   (0x40000000) and below USER_STACK_TOP (0x40200000).
   Headers are placed at the start so PT_LOAD covers the ELF header
   pages cleanly. */
ENTRY(_start)

PHDRS {
    text PT_LOAD FLAGS(5);   /* R-X */
    data PT_LOAD FLAGS(6);   /* RW- */
}

SECTIONS {
    . = 0x40100000;
    .text   : { *(.text*)   } :text
    .rodata : { *(.rodata*) } :text

    . = ALIGN(0x1000);
    .data   : { *(.data*)   } :data
    .bss    : { *(.bss*)    } :data

    /DISCARD/ : { *(.note.*) *(.comment) *(.eh_frame*) }
}
