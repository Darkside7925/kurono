; ═══════════════════════════════════════════════════════════════════════════
;  x86_64 SYSCALL fast-path entry (Linux ABI)
;
;  On entry:
;    CPL=0, RCX = user RIP, R11 = user RFLAGS, RSP = user RSP (UNCHANGED)
;    syscall nr in RAX, args in RDI, RSI, RDX, R10, R8, R9.
;
;  We:
;    1. Stash user RSP into g_user_syscall_rsp_save.
;    2. Load kernel RSP from g_kernel_syscall_rsp.
;    3. Save GPRs that we / the C handler may clobber.
;    4. Call SyscallEntryX64Handler(nr, a0..a5, &saved_user_rip, &saved_user_rflags).
;       (We use a flat struct passed in registers for simplicity  -  see below.)
;    5. Restore GPRs, restore user RSP, sysretq back to user (RCX=user RIP, R11=user RFLAGS).
;
;  The handler returns a 64-bit value that we place in RAX (syscall result).
; ═══════════════════════════════════════════════════════════════════════════

[BITS 64]

extern SyscallEntryX64Handler            ; int64_t (*)(uint64_t nr, uint64_t a0..a5)
extern g_kernel_syscall_rsp              ; uint64_t  -  kernel stack to switch to
extern g_user_syscall_rsp_save           ; uint64_t  -  stash user rsp
extern g_user_syscall_rip_save           ; uint64_t  -  stash user rip (rcx)
extern g_user_syscall_rflags_save        ; uint64_t  -  stash user rflags (r11)

global syscall_entry_x64
syscall_entry_x64:
    ; Stash user state.  We can't touch the stack yet  -  RSP is still user's.
    mov     qword [rel g_user_syscall_rsp_save],    rsp
    mov     qword [rel g_user_syscall_rip_save],    rcx
    mov     qword [rel g_user_syscall_rflags_save], r11

    ; Switch to kernel stack.
    mov     rsp, qword [rel g_kernel_syscall_rsp]

    ; Save callee-clobbered registers we need to preserve for the user.
    ; SysV ABI: caller-saved are rax, rcx, rdx, rsi, rdi, r8-r11.
    ; We'll preserve everything to be safe.
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15

    ; the linux x86_64 syscall abi requires the kernel to preserve ALL
    ; gp registers except rax (result), rcx and r11 (clobbered by the
    ; syscall instruction itself).  musl keeps live values in r8/r10/etc
    ; across a syscall (e.g. __stdout_write holds the FILE* in r8 across
    ; its ioctl probe), so we must save+restore the arg registers too,
    ; not just the sysv callee-saved set (satoru)
    push    rdi
    push    rsi
    push    rdx
    push    r8
    push    r9
    push    r10

    ; Build the call: SyscallEntryX64Handler(nr, a0,a1,a2,a3,a4,a5)
    ; SysV ABI passes: rdi, rsi, rdx, rcx, r8, r9 as first 6 args.
    ; Linux x86_64 syscall ABI uses: rdi, rsi, rdx, r10, r8, r9 for args.
    ; nr is in rax.
    ;
    ; Map: handler(arg1=nr, arg2=a0, arg3=a1, arg4=a2, arg5=a3, arg6=a4)
    ; ... then we'd need to pass a5 on the stack. To keep things simple
    ; we cap at 6 args (nr + 5).  The 6th syscall arg (r9) is rare in
    ; early CPython startup; we can extend later by pushing on stack.
    ;
    ; Final arg map for the C handler (declared as
    ;   int64_t SyscallEntryX64Handler(uint64_t nr,
    ;                                  uint64_t a0, uint64_t a1,
    ;                                  uint64_t a2, uint64_t a3,
    ;                                  uint64_t a4, uint64_t a5);):
    ;   rdi = nr   (rax)
    ;   rsi = a0   (rdi)
    ;   rdx = a1   (rsi)
    ;   rcx = a2   (rdx)
    ;   r8  = a3   (r10)
    ;   r9  = a4   (r8)
    ;   stack[0] = a5 (r9)

    ; Save originals into temps before scribbling.
    mov     r12, rdi          ; r12 = a0
    mov     r13, rsi          ; r13 = a1
    mov     r14, rdx          ; r14 = a2
    mov     r15, r10          ; r15 = a3
    mov     rbx, r8           ; rbx = a4
    mov     rbp, r9           ; rbp = a5

    ; Now load ABI args.
    mov     rdi, rax          ; nr
    mov     rsi, r12          ; a0
    mov     rdx, r13          ; a1
    mov     rcx, r14          ; a2
    mov     r8,  r15          ; a3
    mov     r9,  rbx          ; a4
    push    rbp               ; a5 on stack
    sub     rsp, 8            ; align (16-byte boundary before call)

    cld
    call    SyscallEntryX64Handler

    add     rsp, 16           ; pop a5 + alignment

    ; restore the user arg registers (reverse push order) so r8/r10/etc
    ; survive the syscall as the linux abi guarantees (satoru)
    pop     r10
    pop     r9
    pop     r8
    pop     rdx
    pop     rsi
    pop     rdi

    ; Restore preserved regs.
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx

    ; Restore user RIP and RFLAGS into rcx/r11 for sysretq.
    mov     rcx, qword [rel g_user_syscall_rip_save]
    mov     r11, qword [rel g_user_syscall_rflags_save]

    ; Restore user RSP.
    mov     rsp, qword [rel g_user_syscall_rsp_save]

    ; rax already holds the return value from the C handler.
    o64 sysret
