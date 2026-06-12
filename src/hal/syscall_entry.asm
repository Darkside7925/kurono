; ═══════════════════════════════════════════════════════════════════════════
;  x86_64 SYSCALL fast-path entry (Linux ABI)
;
;  On entry:
;    CPL=0, RCX = user RIP, R11 = user RFLAGS, RSP = user RSP (UNCHANGED),
;    IF cleared by SFMASK.  syscall nr in RAX, args in RDI, RSI, RDX, R10, R8, R9.
;
;  We build a full InterruptFrame on the kernel stack  -  byte-identical to the
;  one the int 0x80 / irq stubs build  -  and hand it to a C handler. that lets a
;  syscall switch tasks the same way int 0x80 does: the handler rewrites the
;  frame in place (futex block, thread exit, clone), and we IRETQ to whatever
;  task the frame now describes instead of SYSRET-ing back to the caller. a
;  non-switching syscall just IRETQs back to the original task with rax set.
;
;  InterruptFrame layout (see hal.h, packed, low→high address):
;    cr2, r15,r14,r13,r12,r11,r10,r9,r8, rbp,rdi,rsi,rdx,rcx,rbx,rax,
;    vector, error_code, rip, cs, rflags, rsp, ss      (23 qwords = 184 bytes)
;  the bottom five (rip,cs,rflags,rsp,ss) are exactly the hardware IRETQ frame.
; ═══════════════════════════════════════════════════════════════════════════

[BITS 64]

extern SyscallEntryX64FrameHandler       ; void (*)(InterruptFrame*)  -  fills rax, may switch
; the kernel stack now comes from this cpu's per-cpu block via gs (KERNEL_GS_BASE
; = &PerCpu) after swapgs, so two cores can syscall at once without sharing one
; global stack. PerCpu offset 0 = user-rsp scratch, offset 8 = kernel rsp. (satoru)

; ring-3 selectors (gdt: user code 0x20|3, user data 0x18|3). (satoru)
USER_CS equ 0x23
USER_SS equ 0x1B

global syscall_entry_x64
syscall_entry_x64:
    ; swapgs brings this cpu's PerCpu pointer into gs, then stash the user rsp and
    ; switch to this cpu's kernel stack. we can't touch the stack until rsp points
    ; at kernel memory. SFMASK cleared IF, so no irq can see the swapped gs. (satoru)
    swapgs
    mov     [gs:0], rsp                          ; PerCpu.user_rsp_save
    mov     rsp, [gs:8]                          ; PerCpu.kernel_rsp (this cpu)

    ; build the InterruptFrame top-down (push writes high address first, so the
    ; first push is the last struct field, ss). every gp reg is captured live
    ; and untouched here  -  in particular r9 still holds musl's clone child-fn,
    ; and rcx/r11 still hold the user rip/rflags. (satoru)
    push    USER_SS                              ; ss
    push    qword [gs:0]                          ; rsp (user, from PerCpu.user_rsp_save)
    push    r11                                  ; rflags (user, in r11)
    push    USER_CS                              ; cs
    push    rcx                                  ; rip (user, in rcx)
    push    0                                    ; error_code
    push    0x80                                 ; vector (cosmetic  -  mark syscall)
    push    rax                                  ; rax (syscall nr; handler overwrites with result)
    push    rbx                                  ; rbx
    push    rcx                                  ; rcx
    push    rdx                                  ; rdx
    push    rsi                                  ; rsi
    push    rdi                                  ; rdi
    push    rbp                                  ; rbp
    push    r8                                   ; r8
    push    r9                                   ; r9  (pristine  -  clone child start fn)
    push    r10                                  ; r10
    push    r11                                  ; r11
    push    r12                                  ; r12
    push    r13                                  ; r13
    push    r14                                  ; r14
    push    r15                                  ; r15
    mov     rax, cr2
    push    rax                                  ; cr2 (offset 0  -  rsp now = &frame)

    ; hand &frame to the C handler. the kernel stack base is 16-aligned and the
    ; frame is 184 bytes (≡8 mod 16), so one extra sub aligns the call site. (satoru)
    mov     rdi, rsp
    sub     rsp, 8
    cld
    call    SyscallEntryX64FrameHandler
    add     rsp, 8                               ; rsp = &frame again

    ; restore every gp reg from the (possibly rewritten) frame, then IRETQ the
    ; bottom five fields. cr2 is not restorable, so skip it. (satoru)
    add     rsp, 8                               ; skip cr2
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     r11
    pop     r10
    pop     r9
    pop     r8
    pop     rbp
    pop     rdi
    pop     rsi
    pop     rdx
    pop     rcx
    pop     rbx
    pop     rax
    add     rsp, 16                              ; skip vector + error_code → rsp = &frame.rip
    swapgs                                        ; restore the user gs before returning (satoru)
    iretq
