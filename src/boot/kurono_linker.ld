OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS
{
    /* Discard notes so .mboot is the very first data in the file */
    /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame) }

    . = 0x100000;
    kernel_start = .;
    
    .text BLOCK(4K) : ALIGN(4K)
    {
        *(.mboot)
        *(.multiboot)
        *(.text)
    }
    
    .rodata BLOCK(4K) : ALIGN(4K)
    {
        *(.rodata)
        *(.rodata.*)
    }
    
    .data BLOCK(4K) : ALIGN(4K)
    {
        *(.data)
        *(.data.*)
    }
    kernel_data_end = .;
    
    .bss BLOCK(4K) : ALIGN(4K)
    {
        kernel_bss_start = .;
        *(COMMON)
        *(.bss)
        *(.stk)
        kernel_bss_end = .;

        /* ── Page tables ──────────────────────────────────────────────
           Placed INSIDE the .bss LOAD segment so GRUB *definitely*
           allocates physical memory for them (critical on UEFI HW).
           They live AFTER kernel_bss_end so the BSS-zero loop in
           kurono_boot.asm does NOT clobber them once filled.         */
        . = ALIGN(4K);
        *(.boot_tables)
    }
    
    . = ALIGN(4K);
    kernel_end = .;
}