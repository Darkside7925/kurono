; ═══════════════════════════════════════════════════════════════════════════
;  Kurono OS - Cooperative Context Switch (x86_64)
;
;  void scheduler_switch_to(uint64_t* prev_saved_rsp,
;                           uint64_t  next_saved_rsp);
;
;  Saves the current callee-saved register set (rbx, rbp, r12, r13, r14,
;  r15, rflags) on the current stack, stores the resulting RSP into
;  *prev_saved_rsp, then loads RSP from next_saved_rsp and pops the
;  callee-saved set + rflags before returning to the next process's
;  saved RIP.
;
;  Per System V AMD64 ABI, the caller is responsible for saving the
;  caller-saved set across this call, so we only need callee-saved.
;
;  When a process is launched for the first time, its kernel stack is
;  primed by Scheduler::SeedKernelStack() such that this `ret`
;  pops the entry function's address, switching cleanly to that
;  function with a freshly cli/sti'd rflags.
; ═══════════════════════════════════════════════════════════════════════════

[BITS 64]
section .text

; context switch. if u change the order of anything in here, firefox will crash 5% of the time and u will never find out why
global scheduler_switch_to
scheduler_switch_to:
    ; rdi = uint64_t* prev_saved_rsp   (where to write our rsp)
    ; rsi = uint64_t   next_saved_rsp  (rsp to switch to)
    pushfq
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    ; persist current rsp into *prev_saved_rsp (if non-null - initial
    ; bootstrap from kernel_main passes nullptr to discard).
    test rdi, rdi
    jz   .no_save
    mov  [rdi], rsp
.no_save:

    mov  rsp, rsi

    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  rbx
    pop  rbp
    popfq
    ; ── last-instant return-address guard (the ring-0 RIP=0x3 corruptor) ──
    ; A C-side check in perform_switch fired ZERO times across 20 boots while a
    ; RIP=0x3 #UD still happened, which proves the slot is clean at check time
    ; and garbage by the time we get here: another cpu writes it inside that
    ; window. Checking HERE narrows the window to these two instructions, the
    ; tightest it can be without a hardware watchpoint. Nothing below 1MB is
    ; kernel text (that range is real-mode/BIOS), so this cannot false-positive.
    ; Falls through to `ret` on the overwhelmingly common good path: one cmp and
    ; a not-taken branch per switch. (satoru)
    cmp  qword [rsp], 0x100000
    jb   .ret_corrupt
    ret
.ret_corrupt:
    ; hand the forensics to C while the evidence is still live. rdi = the bad
    ; value we were about to jump to, rsi = the stack slot it came from. does
    ; not return. (satoru)
    mov  rdi, [rsp]
    mov  rsi, rsp
    extern kp_ret_corrupt_report
    call kp_ret_corrupt_report
.hang:
    cli
    hlt
    jmp  .hang

; ═══════════════════════════════════════════════════════════════════════════
;  void scheduler_jump_to(uint64_t saved_rsp);
;
;  One-way jump used to enter the very first scheduled process from the
;  kernel boot stack.  Identical to scheduler_switch_to with rdi = NULL,
;  but kept as a separate symbol for clarity at the call site.
; ═══════════════════════════════════════════════════════════════════════════
global scheduler_jump_to
scheduler_jump_to:
    mov  rsp, rdi
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  rbx
    pop  rbp
    popfq
    ret

; ═══════════════════════════════════════════════════════════════════════════
;  void ap_enter_user_frame(InterruptFrame* f)   - never returns
;
;  smp thread dispatch: an application processor resumes a claimed user
;  thread from its saved InterruptFrame. mirrors the isr_common restore
;  path exactly: point rsp at the frame, skip cr2, pop the 15 gprs, skip
;  vector+error, iretq into ring-3. the caller (LoadUserFrame) has already
;  set cr3, tss.rsp0/gs:8, fs base and the fpu state. cli first so no irq
;  lands between the stack pivot and the iretq; the frame's rflags carries
;  IF=1 so interrupts resume with the thread. (satoru)
; ═══════════════════════════════════════════════════════════════════════════
global ap_enter_user_frame
ap_enter_user_frame:
    ; [ubchk] exit trace, hal.cpp (satoru)
    extern ub_trace_isr_exit
    mov  rbx, rdi
    and  rsp, ~0xF
    call ub_trace_isr_exit
    mov  rdi, rbx
    cli
    mov  rsp, rdi        ; rsp -> InterruptFrame (cr2 first) (satoru)
    add  rsp, 8          ; skip cr2 (satoru)
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  r11
    pop  r10
    pop  r9
    pop  r8
    pop  rbp
    pop  rdi
    pop  rsi
    pop  rdx
    pop  rcx
    pop  rbx
    pop  rax
    add  rsp, 16         ; skip vector + error code (satoru)
    iretq
; end (satoru)
