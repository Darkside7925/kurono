[BITS 64]
section .text

global UserspaceEnter
global UserspaceResume

; int UserspaceEnter(uint64_t rip, uint64_t rsp, UserspaceReturnContext* ctx)
;   rdi = rip, rsi = user rsp, rdx = ctx
UserspaceEnter:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    mov [rdx + 0], rsp
    lea rax, [rel .resume_from_user]
    mov [rdx + 8], rax

    mov ax, 0x1B
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push qword 0x1B
    push rsi
    pushfq
    pop rax
    or rax, 0x202
    and rax, ~0x3000
    push rax
    push qword 0x23
    push rdi
    iretq

.resume_from_user:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov eax, [rdx + 16]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret

; void UserspaceResume(UserspaceReturnContext* ctx, int exit_code)
;   rdi = ctx, rsi = exit_code
UserspaceResume:
    mov [rdi + 16], esi
    mov rsp, [rdi + 0]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov rax, [rdi + 8]
    mov rdx, rdi
    jmp rax