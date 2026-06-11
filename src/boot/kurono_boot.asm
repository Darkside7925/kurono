; ═══════════════════════════════════════════════════════════════════════════
;  Kurono OS — 64-bit Bootloader (x86_64 Long Mode)
;  Multiboot1 → Protected Mode → Long Mode trampoline
;
;  Flow:
;    1. GRUB/QEMU loads us in 32-bit protected mode (Multiboot spec)
;    2. We build identity-mapped page tables (2MB pages, 16 GB coverage)
;    3. Enable PAE → Long Mode (IA32_EFER.LME) → Paging
;    4. Far-jump to 64-bit code segment
;    5. Clear BSS, call kernel_main(magic, mb_info_addr)
; ═══════════════════════════════════════════════════════════════════════════

; ── Multiboot Constants ──────────────────────────────────────────────────
MBALIGN    equ  1 << 0          ; align loaded modules on page boundaries
MEMINFO    equ  1 << 1          ; provide memory map
; NOTE: VIDEO (bit 2) is NOT set — QEMU's multiboot loader doesn't support
; VBE mode requests (causes "multiboot knows VBE. we don't" abort).
; We program BGA directly in the kernel instead.
AOUTKLUDGE equ  1 << 16         ; address fields in header are valid
FLAGS      equ  MBALIGN | MEMINFO | AOUTKLUDGE
MAGIC      equ  0x1BADB002      ; multiboot magic
CHECKSUM   equ -(MAGIC + FLAGS)

; ── Long Mode Constants ──────────────────────────────────────────────────
MSR_EFER     equ  0xC0000080    ; Extended Feature Enable Register
EFER_LME     equ  1 << 8       ; Long Mode Enable
CR0_PG       equ  1 << 31      ; Paging
CR4_PAE      equ  1 << 5       ; Physical Address Extension

; Number of Page Directories (each covers 1 GB via 512 × 2 MB pages)
NUM_PDS      equ  16            ; 16 GB identity map

; ═══════════════════════════════════════════════════════════════════════════
;  Multiboot Header — must appear in first 8 KiB of the binary
; ═══════════════════════════════════════════════════════════════════════════
extern kernel_data_end

section .mboot
align 4
mboot_header:
    dd MAGIC
    dd FLAGS
    dd CHECKSUM
    dd mboot_header              ; header_addr
    dd mboot_header              ; load_addr (same — header is first thing loaded)
    dd kernel_data_end           ; load_end_addr
    dd kernel_bss_end            ; bss_end_addr
    dd _start                    ; entry_addr
    ; VBE fields omitted — BGA is programmed directly by the kernel

; ═══════════════════════════════════════════════════════════════════════════
;  64-bit GDT (placed in .rodata so it's always accessible)
; ═══════════════════════════════════════════════════════════════════════════
section .rodata
align 16
gdt64:
    dq 0                                ; 0x00 — null descriptor
.code: equ $ - gdt64
    dq 0x00AF9A000000FFFF               ; 0x08 — 64-bit code  (L=1 D=0 P=1 DPL=0)
.data: equ $ - gdt64
    dq 0x00CF92000000FFFF               ; 0x10 — data          (G=1 DB=1 P=1 DPL=0)
gdt64_end:

; 32-bit GDT pointer (used before we switch to long mode)
align 4
gdt64_ptr32:
    dw gdt64_end - gdt64 - 1           ; limit
    dd gdt64                            ; 32-bit base address

; ═══════════════════════════════════════════════════════════════════════════
;  Page Tables — in .boot_tables (NOT inside .bss)
;  Paging is active before BSS is cleared, so these must survive the clear.
; ═══════════════════════════════════════════════════════════════════════════
section .boot_tables nobits alloc write
align 4096
pml4:           resb 4096               ; Page Map Level 4
pdpt:           resb 4096               ; Page Directory Pointer Table
pd_tables:      resb 4096 * NUM_PDS     ; 16 Page Directories → 16 GB

; ═══════════════════════════════════════════════════════════════════════════
;  Stack — in .stk (zeroed with BSS, that's fine)
; ═══════════════════════════════════════════════════════════════════════════
section .stk nobits alloc write
align 16
stack_bottom:
    resb 65536                          ; 64 KiB kernel stack
stack_top:

; ═══════════════════════════════════════════════════════════════════════════
;  32-bit Entry Point — Multiboot hands control here
; ═══════════════════════════════════════════════════════════════════════════
section .text
global _start
extern kernel_main
extern kernel_bss_start
extern kernel_bss_end

[BITS 32]
_start:
    ; ── Save multiboot info (EAX = magic, EBX = info struct address) ──
    mov esi, eax                        ; ESI ← magic
    mov edi, ebx                        ; EDI ← multiboot info pointer

    ; ══════════════════════════════════════════════════════════════════════
    ;  Build identity-mapped page tables:  0 → 16 GB  (2 MB pages)
    ;
    ;  PML4[0]        → PDPT
    ;  PDPT[0..15]    → PD[0..15]
    ;  PD[i][0..511]  → 2 MB page at (i × 1 GB + j × 2 MB)
    ; ══════════════════════════════════════════════════════════════════════

    ; ── Zero PML4 ──
    mov ecx, 4096 / 4
    xor eax, eax
    mov ebx, pml4
.zero_pml4:
    mov [ebx], eax
    add ebx, 4
    dec ecx
    jnz .zero_pml4

    ; ── Zero PDPT ──
    mov ecx, 4096 / 4
    mov ebx, pdpt
.zero_pdpt:
    mov [ebx], eax
    add ebx, 4
    dec ecx
    jnz .zero_pdpt

    ; ── PML4[0] → PDPT  (Present + Writable) ──
    mov eax, pdpt
    or  eax, 0x03
    mov [pml4], eax
    mov dword [pml4 + 4], 0            ; upper 32 bits = 0

    ; ── PDPT[0..NUM_PDS-1] → PD tables ──
    lea ebx, [pd_tables]
    xor ecx, ecx
.setup_pdpt:
    mov eax, ebx
    or  eax, 0x03                       ; Present + Writable
    mov [pdpt + ecx * 8], eax
    mov dword [pdpt + ecx * 8 + 4], 0  ; upper 32 = 0
    add ebx, 4096                       ; advance to next PD
    inc ecx
    cmp ecx, NUM_PDS
    jl  .setup_pdpt

    ; ── Fill PD entries (each maps a 2 MB page, PS=1) ──
    ;    0x83 = Present (bit 0) | Writable (bit 1) | Page Size 2 MB (bit 7)
    lea ebx, [pd_tables]
    mov eax, 0x83                       ; phys = 0 | flags
    xor edx, edx                        ; phys upper 32 = 0
    mov ecx, NUM_PDS * 512              ; total 2 MB entries
.fill_pd:
    mov [ebx],     eax
    mov [ebx + 4], edx
    add ebx, 8
    add eax, 0x200000                   ; next 2 MB frame
    adc edx, 0                          ; carry into upper 32 bits
    dec ecx
    jnz .fill_pd

    ; ── DEBUG: page tables built ──

    ; ══════════════════════════════════════════════════════════════════════
    ;  Activate Long Mode
    ; ══════════════════════════════════════════════════════════════════════

    ; ── Load PML4 into CR3 ──
    mov eax, pml4
    mov cr3, eax

    ; ── Enable PAE (CR4 bit 5) ──
    mov eax, cr4
    or  eax, CR4_PAE
    mov cr4, eax

    ; ── Enable Long Mode (IA32_EFER.LME) ──
    mov ecx, MSR_EFER
    rdmsr
    or  eax, EFER_LME
    wrmsr

    ; ── Enable Paging (CR0 bit 31) — activates Long Mode ──
    mov eax, cr0
    or  eax, CR0_PG
    mov cr0, eax

    ; ── Load 64-bit GDT ──
    lgdt [gdt64_ptr32]

    ; ── Far jump to 64-bit code segment (selector 0x08) ──
    jmp 0x08:long_mode_entry

; ═══════════════════════════════════════════════════════════════════════════
;  64-bit Entry Point — CPU is now in Long Mode
; ═══════════════════════════════════════════════════════════════════════════
[BITS 64]
long_mode_entry:
    ; ── Set up data segments ──
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; ── Enable SSE (required for x86-64 ABI / float / optimized stores) ──
    mov rax, cr0
    and ax, 0xFFFB                      ; clear CR0.EM (bit 2)
    or  ax, 0x0002                      ; set   CR0.MP (bit 1)
    mov cr0, rax
    mov rax, cr4
    or  ax, (1 << 9) | (1 << 10)       ; set CR4.OSFXSR (bit 9) + OSXMMEXCPT (bit 10)
    mov cr4, rax

    ; ── Set up 64-bit stack ──
    mov rsp, stack_top

    ; NOTE: BSS is already zeroed by the Multiboot aout-kludge loader.
    ;       Skipping redundant rep stosq to avoid slow emulated 2 GB clear.

    ; ── Prepare kernel_main(uint64_t magic, uint64_t mb_addr) ──
    ;    ESI = magic,  EDI = mb_info (saved in 32-bit code)
    ;    System V AMD64 ABI: RDI = arg1, RSI = arg2
    mov r8d, esi                        ; r8  = magic       (zero-extended)
    mov r9d, edi                        ; r9  = mb_info_ptr (zero-extended)
    mov rdi, r8                         ; arg1 = magic
    mov rsi, r9                         ; arg2 = mb_addr

    ; ── Call 64-bit kernel ──
    call kernel_main

    ; ── Halt forever if kernel returns ──
    cli
.hang:
    hlt
    jmp .hang