OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS
{
    /* Discard notes so .mboot is the very first data in the file */
    /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame) }

    . = 0x100000;
    
    .text BLOCK(4K) : ALIGN(4K)
    {
        *(.mboot)
        *(.multiboot)
        *(.text)
    }
    
    .rodata BLOCK(4K) : ALIGN(4K)
    {
        *(.rodata)
    }
    
    .data BLOCK(4K) : ALIGN(4K)
    {
        *(.data)
    }
    kernel_data_end = .;
    
    .bss BLOCK(4K) : ALIGN(4K)
    {
        kernel_bss_start = .;
        *(COMMON)
        *(.bss)
        *(.stk)
        kernel_bss_end = .;
    }
    
    /* Page tables — NOT cleared with BSS (paging is already active) */
    .boot_tables (NOLOAD) : ALIGN(4K)
    {
        *(.boot_tables)
    }
    
    kernel_start = .;
    
    . = ALIGN(4K);
    kernel_end = .;
}