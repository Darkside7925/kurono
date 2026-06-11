/* Working multiboot linker script for enhanced 180Hz kernel */
ENTRY(_start)

SECTIONS
{
    /* Start at 1MB - standard multiboot location */
    . = 1M;
    
    /* Multiboot header MUST be early in the file */
    .multiboot BLOCK(4K) : ALIGN(4K)
    {
        *(.multiboot)
    }
    
    /* Entry point and code */
    .text BLOCK(4K) : ALIGN(4K)  
    {
        *(.text)
    }

    /* Read-only data */
    .rodata BLOCK(4K) : ALIGN(4K)
    {
        *(.rodata)
    }

    /* Initialized data */
    .data BLOCK(4K) : ALIGN(4K)
    {
        *(.data)
    }

    /* Uninitialized data and stack */
    .bss BLOCK(4K) : ALIGN(4K)
    {
        *(COMMON)
        *(.bss)
    }
}