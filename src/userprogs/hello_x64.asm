; ═══════════════════════════════════════════════════════════════════════════
;  hello_x64.asm - minimal ring-3 user binary using the x86_64 SYSCALL
;  instruction with Linux x86_64 syscall numbers.  Validates the
;  SYSCALL/SYSRET fast path that musl-built CPython will use.
;
;    write(1, msg, len)   nr=1
;    exit(0)              nr=60
; ═══════════════════════════════════════════════════════════════════════════

[BITS 64]

global _start
_start:
    mov     rax, 1          ; nr = write
    mov     rdi, 1          ; fd = stdout
    lea     rsi, [rel msg]  ; buf
    mov     rdx, msg_len    ; count
    syscall

    mov     rax, 60         ; nr = exit
    xor     rdi, rdi        ; status = 0
    syscall

.spin:
    jmp     .spin

section .rodata
msg:    db  "[ring3] x86_64 SYSCALL hello: musl ABI is alive", 10
msg_len equ $ - msg
