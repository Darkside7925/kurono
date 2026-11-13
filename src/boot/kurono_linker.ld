OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

/* two load segments:
   .boot    - the pre-paging stub, its gdt, the boot page tables and the boot
              stack. linked and loaded at 1 mb, and executed there before
              paging exists. small and self contained. (satoru)
   .kernel  - the real kernel. linked at KERNEL_VIRT (default a high-half
              address far above the user space) and loaded at KERNEL_LOAD_PHYS
              (16 mb). the boot stub maps the link range onto the load range
              with 2 mb pages, so the kernel image no longer has to sit at its
              link address (a low-ram machine could not fit that). (satoru)

   USER_IMAGE_CEILING is the old KURONO_KERNEL_BASE: the top of the fixed
   virtual window a low-address ET_EXEC (claude) may occupy. it only informs
   the pmm now; the kernel itself is not linked there anymore. (satoru) */
PHDRS
{
    boot PT_LOAD FLAGS(7);
    kern PT_LOAD FLAGS(7);
}

SECTIONS
{
    /* Discard notes so .mboot is the very first data in the file */
    /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame) }

    . = 0x100000;
    kernel_boot_start = .;

    .boot BLOCK(4K) : ALIGN(4K)
    {
        KEEP(*(.mboot))
        KEEP(*(.multiboot))
        *(.text.boot)
        *(.rodata.boot)
        . = ALIGN(4K);
        KEEP(*(.boot_tables))
        . = ALIGN(16);
        KEEP(*(.stk))
    } :boot
    kernel_boot_end = .;

    . = DEFINED(KERNEL_VIRT) ? KERNEL_VIRT : 0xFFFF800000000000;
    . = ALIGN(2M);
    kernel_start = .;

    /* fixed low physical load address, 2 mb aligned so the high mapping in the
       boot stub is a whole number of 2 mb pages away from the identity map.
       the load addresses of every .kernel section below are derived from this
       plus the section's offset from kernel_start. (satoru) */
    kernel_phys_start = 0x1000000;

    .text ALIGN(4K) : AT(kernel_phys_start + (ADDR(.text) - kernel_start))
    {
        *(.text)
        *(.text.*)
    } :kern

    .rodata ALIGN(4K) : AT(kernel_phys_start + (ADDR(.rodata) - kernel_start))
    {
        *(.rodata)
        *(.rodata.*)
        *(.lrodata)
        *(.lrodata.*)
    } :kern

    /* c++ global constructors (.init_array). the kernel calls these early in
       boot via RunGlobalConstructors()  -  without it, static-init registrations
       (e.g. the audio backends) never run and the subsystem appears absent. (satoru) */
    .init_array ALIGN(8) : AT(kernel_phys_start + (ADDR(.init_array) - kernel_start))
    {
        __init_array_start = .;
        KEEP(*(.init_array))
        KEEP(*(.init_array.*))
        KEEP(*(.ctors))
        __init_array_end = .;
    } :kern

    .data ALIGN(4K) : AT(kernel_phys_start + (ADDR(.data) - kernel_start))
    {
        *(.data)
        *(.data.*)
        *(.ldata)
        *(.ldata.*)
    } :kern
    kernel_data_end = .;

    .bss ALIGN(4K) : AT(kernel_phys_start + (ADDR(.bss) - kernel_start))
    {
        kernel_bss_start = .;
        *(COMMON)
        *(.bss)
        *(.bss.*)
        /* large-model bss: -mcmodel=large emits zero-init globals bigger than
           the small-data threshold into a SEPARATE .lbss section. it must live
           inside the zeroed bss range AND below kernel_end, or large zero-init
           globals (e.g. the wayland client table g_clients) boot as raw garbage
           and the pmm also hands their memory out as free. pull it in here so
           the boot bss-zero loop covers it and the pmm marks it used. (satoru) */
        *(.lbss)
        *(.lbss.*)
        kernel_bss_end = .;
    } :kern

    . = ALIGN(4K);
    kernel_end = .;
    kernel_phys_end = kernel_phys_start + (kernel_end - kernel_start);

    /* top of the fixed virtual window for low-address user images, in the
       kernel's own address space terms. KURONO_KERNEL_BASE still names it on
       the make command line. (satoru) */
    user_image_ceiling = DEFINED(KERNEL_BASE) ? KERNEL_BASE : 0x14000000;
}
