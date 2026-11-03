[BITS 64]
section .text

global UserspaceEnter
global UserspaceResume

; int UserspaceEnter(uint64_t rip, uint64_t rsp, UserspaceReturnContext* ctx,
;                    uint64_t fs_base)
;   rdi = rip, rsi = user rsp, rdx = ctx, rcx = fs_base
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

    ; capture THIS cpu's per-cpu gs base before the selector load below zeroes
    ; it, and stash it in the context so .resume_from_user can put it back.
    ; rdmsr returns in edx:eax, so ctx (rdx) and fs_base (rcx) move out of the
    ; way first. (satoru)
    mov r10, rdx                  ; ctx
    mov r11, rcx                  ; fs_base
    mov ecx, 0xC0000102           ; MSR_KERNEL_GS_BASE - THIS is where the
                                  ; per-cpu pointer lives while the kernel
                                  ; runs here (active gsbase is 0). measured,
                                  ; not assumed: [uentry] showed gsbase=0
                                  ; kgsbase=0x04AC8F00. (satoru)
    rdmsr
    mov [r10 + 24], eax
    mov [r10 + 28], edx
    mov rdx, r10
    mov rcx, r11

    mov ax, 0x1B
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; loading the fs SELECTOR above reset fsbase to the descriptor base (0). a
    ; dynamic pie needs its tls thread pointer live before _start runs, so
    ; program MSR_FS_BASE (0xC0000100) from rcx now, after the selector load and
    ; before iretq. rcx==0 (static binaries) -> leave it; they set their own fs
    ; via arch_prctl. clobbers rax/rcx/rdx, all dead here (ctx already saved). (satoru)
    test rcx, rcx
    jz .no_user_fsbase
    mov rax, rcx
    mov rdx, rcx
    shr rdx, 32
    mov ecx, 0xC0000100
    wrmsr
.no_user_fsbase:

    ; install the per-cpu gs base into KERNEL_GS_BASE. we iretq straight to ring
    ; 3 with no swapgs, so ring 3 runs on the (zeroed) active gsbase while the
    ; per-cpu pointer waits in KERNEL_GS_BASE for syscall_entry_x64's swapgs to
    ; pick it up. without this the first syscall of the process reads its kernel
    ; stack pointer from address 0. (satoru)
    mov eax, [r10 + 24]
    mov edx, [r10 + 28]
    mov ecx, 0xC0000102           ; MSR_KERNEL_GS_BASE
    wrmsr

    push qword 0x1B
    push rsi
    pushfq
    pop rax
    or rax, 0x202
    and rax, ~0x3000
    push rax
    push qword 0x23
    push rdi

    ; ZERO EVERY GP REGISTER BEFORE ENTERING RING 3. two separate reasons, and
    ; the first one is a real bug that cost a day:
    ;
    ; 1. THE SYSV x86-64 ABI SAYS %rdx AT PROCESS ENTRY IS A FUNCTION POINTER
    ;    THE PROGRAM SHOULD REGISTER WITH atexit() (the rtld_fini slot). linux
    ;    hands over 0 for a static binary. we were still carrying the kernel
    ;    `ctx` pointer in rdx, so glibc's __libc_start_main registered a KERNEL
    ;    HEAP ADDRESS as an exit handler and __run_exit_handlers called straight
    ;    into it - a #PF at ring 3 on a supervisor page (err=0x15, cr2==rip) on
    ;    every glibc-static binary, right at exit, after all real work was done.
    ;    musl ignores rtld_fini entirely, which is why our own musl programs
    ;    never tripped it and prebuilt busybox did.
    ;
    ; 2. anything left here is a kernel pointer handed to userspace for free.
    ;
    ; the iret frame is already pushed, so rsp/rip/flags are safe; every other
    ; register is dead. 32-bit xor clears the full 64-bit register. (satoru)
    xor eax,  eax
    xor ebx,  ebx
    xor ecx,  ecx
    xor edx,  edx
    xor esi,  esi
    xor edi,  edi
    xor ebp,  ebp
    xor r8d,  r8d
    xor r9d,  r9d
    xor r10d, r10d
    xor r11d, r11d
    xor r12d, r12d
    xor r13d, r13d
    xor r14d, r14d
    xor r15d, r15d
    iretq

.resume_from_user:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    ; the gs selector load just zeroed gsbase, so put this cpu's per-cpu pointer
    ; back before returning into kernel c code. we arrive here by longjmp from a
    ; syscall/fault handler that already swapgs'd, so nothing else will restore
    ; it - leaving it zero means the kernel runs with no per-cpu base and the
    ; NEXT process launched on this cpu dies in syscall entry. rdx is the ctx
    ; and wrmsr clobbers edx, so move it aside first. (satoru)
    mov r10, rdx
    mov eax, [r10 + 24]
    mov edx, [r10 + 28]
    mov ecx, 0xC0000102           ; MSR_KERNEL_GS_BASE
    wrmsr
    mov eax, [r10 + 16]
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