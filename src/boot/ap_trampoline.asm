; ═══════════════════════════════════════════════════════════════════════════
;  kurono ap trampoline  -  brings an application processor from its real-mode
;  SIPI entry up to 64-bit long mode on the kernel's *shared* page tables, then
;  jumps to the C ap_entry().
;
;  assembled flat (nasm -f bin) and copied to physical 0x8000 at runtime; the
;  SIPI start vector is therefore 0x08 (0x8000 >> 12). the bsp fills a small
;  patch area at physical 0x9000 before each SIPI:
;      0x9000  qword  cr3 (shared pml4, low 32 bits used)
;      0x9008  10byte kernel gdt pointer (limit:2 + base:8, from sgdt)
;      0x9018  qword  &ap_entry  (64-bit C entry)
;      0x9020  qword  this ap's stack top
;  stages: real mode -> 32-bit protected (own flat gdt) -> enable PAE/LME/PG on
;  the shared cr3 -> load the kernel gdt -> 64-bit -> set stack -> call C. (satoru)
; ═══════════════════════════════════════════════════════════════════════════

P_CR3    equ 0x9000
P_GDTPTR equ 0x9008
P_ENTRY  equ 0x9018
P_STACK  equ 0x9020

org 0x8000
bits 16
ap_start:
    cli
    cld
    xor ax, ax
    mov ds, ax                 ; ds=0 so absolute [0x8xxx] references resolve (satoru)
    mov es, ax
    mov ss, ax

    lgdt [ap_gdt32_ptr]        ; flat 32-bit gdt embedded below (satoru)
    mov eax, cr0
    or  eax, 1                 ; CR0.PE (satoru)
    mov cr0, eax
    jmp 0x08:ap_pm32           ; flush + load 32-bit code selector (satoru)

bits 32
ap_pm32:
    mov ax, 0x10               ; flat 32-bit data (satoru)
    mov ds, ax
    mov es, ax
    mov ss, ax

    mov eax, cr4
    or  eax, (1 << 5)          ; CR4.PAE  -  before cr3, per intel sdm (satoru)
    mov cr4, eax

    mov eax, [P_CR3]           ; shared pml4 from the patch area (satoru)
    mov cr3, eax

    mov ecx, 0xC0000080        ; IA32_EFER (satoru)
    rdmsr
    or  eax, (1 << 8)          ; EFER.LME (satoru)
    wrmsr

    mov eax, cr0
    or  eax, (1 << 31)         ; CR0.PG (satoru)
    mov cr0, eax

    lgdt [P_GDTPTR]            ; kernel 64-bit gdt (base < 4gb) (satoru)
    jmp 0x08:ap_lm64           ; kernel 64-bit code selector (satoru)

bits 64
ap_lm64:
    mov ax, 0x10               ; kernel data selector (satoru)
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov rax, cr0               ; enable SSE for the x86-64 abi (satoru)
    and ax, 0xFFFB             ; clear CR0.EM (satoru)
    or  ax, 0x0002             ; set CR0.MP (satoru)
    mov cr0, rax
    mov rax, cr4
    or  rax, (1 << 9) | (1 << 10)   ; CR4.OSFXSR + OSXMMEXCPT (satoru)
    mov cr4, rax

    mov rsp, [P_STACK]         ; this ap's stack top (satoru)
    mov rax, [P_ENTRY]
    call rax                   ; ap_entry()  -  never returns (satoru)
.hang:
    cli
    hlt
    jmp .hang

; ── flat 32-bit gdt used only for the real-mode -> protected-mode hop ────────
align 8
ap_gdt32:
    dq 0x0000000000000000      ; null (satoru)
    dq 0x00CF9A000000FFFF      ; 0x08: 32-bit code, base 0, limit 4g (satoru)
    dq 0x00CF92000000FFFF      ; 0x10: 32-bit data, base 0, limit 4g (satoru)
ap_gdt32_ptr:
    dw (ap_gdt32_ptr - ap_gdt32 - 1)
    dd ap_gdt32

; pad to one page so the copy + patch-area math stay clean (satoru)
times 4096 - ($ - ap_start) db 0
; end (satoru)
