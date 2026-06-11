; Multiboot header for Enhanced Kurono OS 180Hz Kernel
; Ensures proper boot detection by QEMU and bootloaders

MULTIBOOT_HEADER_MAGIC    equ 0x1BADB002
MULTIBOOT_HEADER_FLAGS    equ 0x00010003  ; Page-aligned modules + memory info + video
MULTIBOOT_CHECKSUM        equ -(MULTIBOOT_HEADER_MAGIC + MULTIBOOT_HEADER_FLAGS)

section .multiboot
align 4
    dd MULTIBOOT_HEADER_MAGIC
    dd MULTIBOOT_HEADER_FLAGS
    dd MULTIBOOT_CHECKSUM
    
    ; Address fields (unused for ELF)
    dd 0  ; header_addr
    dd 0  ; load_addr  
    dd 0  ; load_end_addr
    dd 0  ; bss_end_addr
    dd 0  ; entry_addr
    
    ; Video mode fields for graphics
    dd 0  ; mode_type (0 = linear graphics)
    dd 1024  ; width
    dd 768   ; height  
    dd 32    ; depth

section .text
global _start
extern _kernel_main

_start:
    ; Set up stack for enhanced kernel
    mov esp, stack_top
    
    ; Clear direction flag
    cld
    
    ; Push multiboot info for enhanced graphics initialization
    push ebx  ; multiboot_info pointer
    push eax  ; multiboot_magic
    
    ; Call enhanced kernel main with 180Hz graphics
    call _kernel_main
    
    ; If kernel returns, halt system
    cli
.hang:
    hlt
    jmp .hang

section .bss
align 16
stack_bottom:
    resb 16384  ; 16KB stack for enhanced drivers
stack_top: