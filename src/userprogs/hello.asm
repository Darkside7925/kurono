; src/userprogs/hello.asm - minimal x86_64 user-mode program.
;   Uses int 0x80 with i386 syscall numbers, which is the path Kurono's
;   kernel currently dispatches through LinuxSyscall::Dispatch.
;   The kernel runs user code in 64-bit long mode; 32-bit register
;   forms are valid encodings, so this works without compat segments.

[BITS 64]

global _start

section .text
_start:
    ; sys_write(fd=1, buf=msg, count=msg_len)
    mov eax, 4               ; SYS_write (i386)
    mov ebx, 1               ; stdout
    mov ecx, msg
    mov edx, msg_len
    int 0x80

    ; sys_exit(0)
    mov eax, 1               ; SYS_exit (i386)
    xor ebx, ebx
    int 0x80

    ; should never reach here
.spin:
    jmp .spin

section .rodata
msg:     db "[ring3] ELF64 hello: userspace runtime is alive", 10
msg_len  equ $ - msg
