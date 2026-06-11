; Corrected multiboot entry point for Kurono OS Enhanced
section .text
global _start
extern _kernel_main

_start:
    cli                             ; Disable interrupts
    mov esp, stack_top              ; Set up stack
    
    ; Multiboot information is in EBX
    ; Multiboot magic is in EAX  
    push ebx                        ; Push multiboot info
    push eax                        ; Push multiboot magic
    
    ; Call main kernel function (with underscore)
    call _kernel_main
    
    ; If kernel returns, halt
    cli
.hang:
    hlt
    jmp .hang

section .bss
align 16
stack_bottom:
    resb 16384                      ; 16KB stack
stack_top: