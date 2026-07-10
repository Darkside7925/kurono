; ═══════════════════════════════════════════════════════════════════════════
;  x86_64 SYSCALL fast-path entry (Linux ABI)
;
;  On entry:
;    CPL=0, RCX = user RIP, R11 = user RFLAGS, RSP = user RSP (UNCHANGED),
;    IF cleared by SFMASK.  syscall nr in RAX, args in RDI, RSI, RDX, R10, R8, R9.
;
;  We build a full InterruptFrame on the kernel stack - byte-identical to the
;  one the int 0x80 / irq stubs build - and hand it to a C handler. that lets a
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

extern SyscallEntryX64FrameHandler       ; void (*)(InterruptFrame*) - fills rax, may switch
extern sched_current_task_raw            ; void* () - current task ptr, to detect a switch (satoru)
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
    ; and untouched here - in particular r9 still holds musl's clone child-fn,
    ; and rcx/r11 still hold the user rip/rflags. (satoru)
    push    USER_SS                              ; ss
    push    qword [gs:0]                          ; rsp (user, from PerCpu.user_rsp_save)
    push    r11                                  ; rflags (user, in r11)
    push    USER_CS                              ; cs
    push    rcx                                  ; rip (user, in rcx)
    push    0                                    ; error_code
    push    0x80                                 ; vector (cosmetic - mark syscall)
    push    rax                                  ; rax (syscall nr; handler overwrites with result)
    push    rbx                                  ; rbx
    push    rcx                                  ; rcx
    push    rdx                                  ; rdx
    push    rsi                                  ; rsi
    push    rdi                                  ; rdi
    push    rbp                                  ; rbp
    push    r8                                   ; r8
    push    r9                                   ; r9  (pristine - clone child start fn)
    push    r10                                  ; r10
    push    r11                                  ; r11
    push    r12                                  ; r12
    push    r13                                  ; r13
    push    r14                                  ; r14
    push    r15                                  ; r15
    mov     rax, cr2
    push    rax                                  ; cr2 (offset 0 - rsp now = &frame)

    ; preserve the user's fpu/sse (xmm) state across the whole syscall. kernel
    ; code (memcpy, graphics inline asm) freely clobbers xmm, and the SYSRET/IRETQ
    ; return path does NOT otherwise restore it - which corrupted musl __init_tp's
    ; movups store of the main thread's tcb next/prev and #pf'd pthread_create.
    ;
    ; keep &frame in r12 (frame-saved, so clobbering it here is fine - the exit
    ; pops reload the user's r12 from the frame). carve a 16-aligned 512-byte
    ; fxsave area BELOW the frame; do NOT derive &frame back from rsp afterwards
    ; (the alignment `and` drops a variable 0..15 bytes, which silently shifted
    ; the frame pointer by 8 and made iretq #gp on a corrupt return frame). r13
    ; holds the fxsave area; r14 the pre-handler task. (satoru)
    mov     r12, rsp                             ; r12 = &frame (preserved across calls)
    sub     rsp, 512
    and     rsp, ~0xF                            ; 16-align for fxsave/fxrstor
    mov     r13, rsp                             ; r13 = fxsave area
    fxsave  [r13]                                 ; save PRISTINE user fpu/sse

    ; record the current task BEFORE the handler so we can tell on return whether
    ; it switched tasks (clone/futex/thread-exit) - in which case LoadUserFrame
    ; already loaded the next task's fpu and we must not overwrite it. (satoru)
    call    sched_current_task_raw
    mov     r14, rax                             ; r14 = pre-handler task

    ; hand the true &frame (r12) to the C handler. rsp is 16-aligned (r13), and a
    ; call pushes 8 -> the callee sees the abi-required rsp%16==8. (satoru)
    mov     rdi, r12
    cld
    call    SyscallEntryX64FrameHandler

    ; if the task is unchanged, restore the pristine user fpu state we saved; if
    ; it switched, the new task's fpu is already live (via LoadUserFrame) - skip.
    ; (satoru)
    call    sched_current_task_raw
    cmp     rax, r14
    jne     .skip_fxrstor
    fxrstor [r13]
.skip_fxrstor:

    ; restore rsp to the true &frame (r12), then pop the (possibly rewritten)
    ; frame. cr2 is not restorable, so skip it. (satoru)
    mov     rsp, r12
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
