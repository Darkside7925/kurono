; ═══════════════════════════════════════════════════════════════════════════
;  Kurono OS  -  ISR / IRQ Assembly Stubs (x86_64)
;
;  Each stub saves the full register state into an InterruptFrame on the
;  stack, calls the common C handler, and restores state before iretq.
;
;  Exception vectors 0-31:
;    - Some push an error code automatically (8,10-14,17,21,29,30).
;    - Others do NOT  -  we push a dummy 0 to keep the frame consistent.
;
;  IRQ vectors 32-47:
;    - All push a dummy 0 (hardware interrupts never push error codes).
;
;  The C handler signature is:
;      extern "C" void isr_common_handler(InterruptFrame* frame);
; ═══════════════════════════════════════════════════════════════════════════

[BITS 64]
section .text

extern isr_common_handler

; ─── Macro: ISR stub WITHOUT error code ──────────────────────────────────
; Pushes a dummy 0 error code, then the vector number.
%macro ISR_NOERR 1
global isr_stub_%1
isr_stub_%1:
    push qword 0               ; dummy error code
    push qword %1              ; vector number
    jmp  isr_common
%endmacro

; ─── Macro: ISR stub WITH error code (CPU pushes it for us) ─────────────
; Only pushes the vector number.
%macro ISR_ERR 1
global isr_stub_%1
isr_stub_%1:
    ; error code already on stack from CPU
    push qword %1              ; vector number
    jmp  isr_common
%endmacro

; ─── Exception Stubs (vectors 0-31) ─────────────────────────────────────
ISR_NOERR 0    ; #DE  Divide-by-zero
ISR_NOERR 1    ; #DB  Debug
ISR_NOERR 2    ; NMI
ISR_NOERR 3    ; #BP  Breakpoint
ISR_NOERR 4    ; #OF  Overflow
ISR_NOERR 5    ; #BR  Bound Range Exceeded
ISR_NOERR 6    ; #UD  Invalid Opcode
ISR_NOERR 7    ; #NM  Device Not Available
ISR_ERR   8    ; #DF  Double Fault
ISR_NOERR 9    ; Coprocessor Segment Overrun (legacy)
ISR_ERR   10   ; #TS  Invalid TSS
ISR_ERR   11   ; #NP  Segment Not Present
ISR_ERR   12   ; #SS  Stack-Segment Fault
ISR_ERR   13   ; #GP  General Protection Fault
ISR_ERR   14   ; #PF  Page Fault
ISR_NOERR 15   ; Reserved
ISR_NOERR 16   ; #MF  x87 Floating-Point
ISR_ERR   17   ; #AC  Alignment Check
ISR_NOERR 18   ; #MC  Machine Check
ISR_NOERR 19   ; #XM  SIMD Floating-Point
ISR_NOERR 20   ; #VE  Virtualization
ISR_ERR   21   ; #CP  Control Protection
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29   ; #VC  VMM Communication
ISR_ERR   30   ; #SX  Security Exception
ISR_NOERR 31

; ─── IRQ Stubs (vectors 32-47) ──────────────────────────────────────────
ISR_NOERR 32   ; IRQ0  PIT Timer
ISR_NOERR 33   ; IRQ1  Keyboard
ISR_NOERR 34   ; IRQ2  Cascade
ISR_NOERR 35   ; IRQ3  COM2
ISR_NOERR 36   ; IRQ4  COM1
ISR_NOERR 37   ; IRQ5  LPT2
ISR_NOERR 38   ; IRQ6  Floppy
ISR_NOERR 39   ; IRQ7  Spurious
ISR_NOERR 40   ; IRQ8  CMOS RTC
ISR_NOERR 41   ; IRQ9  Free
ISR_NOERR 42   ; IRQ10 Free
ISR_NOERR 43   ; IRQ11 Free
ISR_NOERR 44   ; IRQ12 Mouse
ISR_NOERR 45   ; IRQ13 FPU
ISR_NOERR 46   ; IRQ14 Primary ATA
ISR_NOERR 47   ; IRQ15 Secondary ATA / Spurious
ISR_NOERR 128  ; User syscall gate
ISR_NOERR 64   ; per-AP LAPIC timer (smp phase 4)
ISR_NOERR 65   ; tlb-shootdown ipi (smp thread dispatch) (satoru)

; ═══════════════════════════════════════════════════════════════════════════
;  Common handler  -  saves all GPRs, calls C, restores, iretq
;
;  Stack layout on entry here (growing downward, RSP points to top):
;    [RSP+ 0]  vector number   (pushed by our stub)
;    [RSP+ 8]  error code      (pushed by CPU or dummy 0)
;    [RSP+16]  RIP             (pushed by CPU)
;    [RSP+24]  CS
;    [RSP+32]  RFLAGS
;    [RSP+40]  RSP             (pre-interrupt)
;    [RSP+48]  SS
;
;  We push 15 GPRs (RAX..R15 minus RSP) → 15*8 = 120 bytes.
;  Then RDI = RSP (pointer to InterruptFrame), call C handler.
; ═══════════════════════════════════════════════════════════════════════════
isr_common:
    ; Save all general-purpose registers
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; Read CR2 (page-fault address) and push it too
    mov  rax, cr2
    push rax

    ; Argument: RDI = pointer to InterruptFrame on stack
    mov  rdi, rsp
    cld                         ; SysV ABI requires DF=0

    call isr_common_handler

    ; Pop CR2 (we don't write it back  -  it's read-only effectively)
    add  rsp, 8

    ; Restore all GPRs
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

    ; Remove vector number and error code from stack
    add  rsp, 16

    iretq

; ═══════════════════════════════════════════════════════════════════════════
;  ISR stub table  -  array of 48 function pointers for C to index
; ═══════════════════════════════════════════════════════════════════════════
section .rodata
global isr_stub_table
isr_stub_table:
%assign i 0
%rep 48
    dq isr_stub_%+i
%assign i i+1
%endrep
