.section .text
.global _start
.type _start, @function
_start:
    movl $stack_top, %esp
    push %ebx
    push %eax
    call kernel_main
    cli
1:  hlt
    jmp 1b
.size _start, . - _start

.section .bss
.align 16
stack_bottom:
.skip 16384 # 16 KB stack
stack_top:
