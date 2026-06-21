; ═══════════════════════════════════════════════════════════════════════════
;  Kurono OS  -  64-bit Bootloader (x86_64 Long Mode)
;  Multiboot2 + Multiboot1 → Protected Mode → Long Mode trampoline
;
;  Flow:
;    1. GRUB loads us in 32-bit protected mode (Multiboot spec)
;    2. We build identity-mapped page tables (2MB pages, high EFI FB coverage)
;    3. Enable PAE → Long Mode (IA32_EFER.LME) → Paging
;    4. Far-jump to 64-bit code segment
;    5. Clear BSS, call kernel_main(magic, mb_info_addr)
;
;  Two multiboot headers are present:
;    • Multiboot2  -  preferred by GRUB on UEFI (much better EFI trampoline)
;    • Multiboot1  -  fallback for BIOS GRUB and QEMU "-kernel" direct load
;
;  Serial debug markers (COM1 @ 115200 8N1):
;    K0 = _start reached, serial initialised
;    K1 = A20 + early FB + CPUID passed
;    K2 = page tables built
;    K3 = about to activate long mode (CR3/EFER/CR0)
;    K4 = 64-bit long mode entered, SSE + PAT + stack ready
;    K5 = BSS zeroed, calling kernel_main
; ═══════════════════════════════════════════════════════════════════════════

; ── Multiboot1 Constants ─────────────────────────────────────────────────
MB1_ALIGN    equ  1 << 0          ; align loaded modules on page boundaries
MB1_MEMINFO  equ  1 << 1          ; provide memory map
MB1_VIDEO    equ  1 << 2          ; request framebuffer from bootloader
MB1_FLAGS    equ  MB1_ALIGN | MB1_MEMINFO | MB1_VIDEO
MB1_MAGIC    equ  0x1BADB002      ; multiboot1 magic
MB1_CHECKSUM equ -(MB1_MAGIC + MB1_FLAGS)

; ── Multiboot2 Constants ─────────────────────────────────────────────────
MB2_MAGIC    equ  0xE85250D6      ; multiboot2 header magic
MB2_ARCH     equ  0               ; architecture: i386 / 32-bit protected mode

; ── Long Mode Constants ──────────────────────────────────────────────────
MSR_EFER     equ  0xC0000080      ; Extended Feature Enable Register
EFER_LME     equ  1 << 8         ; Long Mode Enable
CR0_PG       equ  1 << 31        ; Paging
CR0_NE       equ  1 << 5         ; Numeric Exception (native x87 error reporting)
CR4_PAE      equ  1 << 5         ; Physical Address Extension

; ── Serial Port ──────────────────────────────────────────────────────────
COM1         equ  0x3F8

; Number of Page Directories (each covers 1 GB via 512 × 2 MB pages)
; Real laptops often place the EFI GOP framebuffer far above 16 GB
; (for example 0x4000000000 = 256 GB).  QEMU tends to keep it below 4 GB,
; which is why the old 16 GB map only failed on bare metal.
NUM_PDS      equ  512             ; 512 GB identity map

; ═══════════════════════════════════════════════════════════════════════════
;  Serial output helper  -  works in both [BITS 32] and [BITS 64] modes
;  Clobbers: AL, DX
; ═══════════════════════════════════════════════════════════════════════════
%macro SERIAL_CHAR 1
%%wait:
    mov dx, COM1 + 5            ; Line Status Register
    in al, dx
    test al, 0x20               ; TX Holding Register Empty?
    jz %%wait
    mov dx, COM1
    mov al, %1
    out dx, al
%endmacro

; ═══════════════════════════════════════════════════════════════════════════
;  Multiboot2 Header  -  preferred for UEFI GRUB
;  Must be 8-byte aligned and within first 32 KiB of the image.
; ═══════════════════════════════════════════════════════════════════════════
section .mboot

align 8
mb2_hdr_start:
    dd  MB2_MAGIC                                           ; magic
    dd  MB2_ARCH                                            ; architecture
    dd  mb2_hdr_end - mb2_hdr_start                         ; header length
    dd  0 - (MB2_MAGIC + MB2_ARCH + (mb2_hdr_end - mb2_hdr_start))  ; checksum

    ; ── Framebuffer request tag (type 5) ──
    align 8
    dw  5                               ; type  = framebuffer
    dw  1                               ; flags = optional (don't fail boot if unavailable)
    dd  20                              ; size  = 20 bytes
    dd  1024                            ; preferred width
    dd  768                             ; preferred height
    dd  32                              ; preferred depth

    ; ── Information request tag (type 1) ──
    ;    Ask bootloader to provide memory map + framebuffer tags
    align 8
    dw  1                               ; type  = information request
    dw  1                               ; flags = optional (don't fail boot if unavailable)
    dd  24                              ; size  = 8 header + 4 per request type
    dd  4                               ; request: basic meminfo (type 4)
    dd  6                               ; request: memory map   (type 6)
    dd  8                               ; request: framebuffer  (type 8)
    dd  1                               ; request: cmdline      (type 1)

    ; ── Entry address tag (type 3) ── explicit entry for buggy GRUB EFI
    align 8
    dw  3                               ; type  = entry address
    dw  0                               ; flags = required
    dd  12                              ; size  = 12 bytes
    dd  _start                          ; entry_addr (physical)

    ; ── EFI Boot Services tag (type 7) ──
    ;    Tells GRUB to NOT exit EFI boot services and NOT use the
    ;    relocator (64→32 mode switch).  Combined with type 9 below,
    ;    GRUB stays in 64-bit long mode and jumps to _start_efi64.
    ;    On BIOS GRUB this tag is harmlessly ignored.
    align 8
    dw  7                               ; type  = EFI boot services
    dw  1                               ; flags = optional (bit 0)
    dd  8                               ; size  = 8 (header only)

    ; ── EFI amd64 entry address tag (type 9) ──
    ;    64-bit entry point used ONLY on UEFI x86_64 when tag 7 is present.
    ;    Completely bypasses GRUB's fragile 64→32 relocator.
    align 8
    dw  9                               ; type  = EFI amd64 entry address
    dw  1                               ; flags = optional (bit 0)
    dd  12                              ; size  = 12 bytes
    dd  _start_efi64                    ; 64-bit entry point (physical)

    ; ── End tag ──
    align 8
    dw  0                               ; type  = end
    dw  0                               ; flags
    dd  8                               ; size
mb2_hdr_end:

; ═══════════════════════════════════════════════════════════════════════════
;  Multiboot1 Header  -  fallback for BIOS GRUB and QEMU "-kernel"
;  Must be within first 8 KiB of the image.
; ═══════════════════════════════════════════════════════════════════════════
align 4
mb1_header:
    dd  MB1_MAGIC
    dd  MB1_FLAGS
    dd  MB1_CHECKSUM
    ; ── Address fields (unused  -  GRUB uses ELF headers) ──
    dd 0, 0, 0, 0, 0
    ; ── VBE mode request (VIDEO flag set) ──
    dd  0                               ; mode_type: 0 = linear graphics
    dd  1024                            ; width
    dd  768                             ; height
    dd  32                              ; depth (bpp)

; ═══════════════════════════════════════════════════════════════════════════
;  64-bit GDT (placed in .rodata so it's always accessible)
; ═══════════════════════════════════════════════════════════════════════════
section .rodata
align 16
gdt64:
    dq  0                               ; 0x00  -  null descriptor
.code: equ $ - gdt64
    dq  0x00AF9A000000FFFF              ; 0x08  -  64-bit code (L=1 D=0 P=1 DPL=0)
.data: equ $ - gdt64
    dq  0x00CF92000000FFFF              ; 0x10  -  data        (G=1 DB=1 P=1 DPL=0)
gdt64_end:

; 32-bit GDT pointer (used before we switch to long mode)
align 4
gdt64_ptr32:
    dw  gdt64_end - gdt64 - 1          ; limit
    dd  gdt64                           ; 32-bit base address

; 64-bit GDT pointer (reload after entering long mode)
align 8
gdt64_ptr64:
    dw  gdt64_end - gdt64 - 1          ; limit
    dq  gdt64                           ; full 64-bit base address

; ═══════════════════════════════════════════════════════════════════════════
;  Page Tables  -  in .boot_tables (lives INSIDE .bss LOAD segment via linker
;  script, but AFTER kernel_bss_end so BSS zeroing won't clobber them)
; ═══════════════════════════════════════════════════════════════════════════
section .boot_tables nobits alloc write
align 4096
pml4:           resb 4096               ; Page Map Level 4
global pdpt                             ; exported so C can wire extra PDs for >16 GB FBs
pdpt:           resb 4096               ; Page Directory Pointer Table
global pd_tables                        ; exported so C code can remap FB pages
pd_tables:      resb 4096 * NUM_PDS     ; NUM_PDS Page Directories → NUM_PDS GB (512)
; EFI path only: 3 extra PDPTs of 1 GiB pages for PML4[1..3], extending the
; identity map to 2 TiB so high GOP framebuffers (e.g. ~1 TiB at 0xFA10000000
; on some laptops) are mapped  -  the 512 GB PD map above does not reach them. (satoru)
alignb 4096
global efi_hi_pdpts                      ; exported so graphics.cpp can set WC on the 1 GiB FB page
efi_hi_pdpts:   resb 4096 * 3

; ═══════════════════════════════════════════════════════════════════════════
;  Stack  -  in .stk (zeroed with BSS, that's fine)
; ═══════════════════════════════════════════════════════════════════════════
section .stk nobits alloc write
align 16
stack_bottom:
    resb 65536                          ; 64 KiB kernel stack
stack_top:

; ═══════════════════════════════════════════════════════════════════════════
;  32-bit Entry Point  -  Multiboot hands control here
;  State: 32-bit PM, flat 4 GB segments, paging OFF, A20 enabled,
;         EAX = magic, EBX = info pointer, IF=0 (interrupts disabled)
; ═══════════════════════════════════════════════════════════════════════════
section .text
global _start
extern kernel_main
extern kernel_bss_start
extern kernel_bss_end

[BITS 32]
_start:
    ; ── CRITICAL: Disable all interrupt sources immediately ──
    cli
    cld                                     ; DF is UNDEFINED per MB spec  -  MUST clear
                                            ; or rep stosq later zeroes BSS backwards!

    ; ── Save multiboot registers FIRST (before any AL-clobbering I/O) ──
    ;    EAX = multiboot magic  (0x2BADB002 for MB1, 0x36d76289 for MB2)
    ;    EBX = multiboot info struct physical address
    mov esi, eax                        ; ESI ← magic  (UNCORRUPTED)
    mov edi, ebx                        ; EDI ← info pointer

    ; ── Set up a valid stack IMMEDIATELY  -  ESP is UNDEFINED per MB spec ──
    ;    pushfd/popfd in the CPUID check below will fault if ESP is garbage.
    ;    .stk is inside the BSS LOAD segment, so GRUB already allocated RAM.
    mov esp, stack_top

    ; ── Disable NMI (set bit 7 of CMOS address port 0x70) ──
    ;    NMI bypasses CLI.  Must suppress before IDT exists.
    in al, 0x70
    or al, 0x80                         ; Bit 7 = NMI disable
    out 0x70, al
    in al, 0x71                         ; Dummy read completes CMOS cycle

    ; ══════════════════════════════════════════════════════════════════════
    ;  Initialise COM1 serial port  -  earliest possible debug channel.
    ;  115200 baud 8N1.  Works even when display shows nothing.
    ; ══════════════════════════════════════════════════════════════════════
    mov dx, COM1 + 1                    ; Interrupt Enable Register
    xor al, al
    out dx, al                          ; Disable all UART interrupts
    mov dx, COM1 + 3                    ; Line Control Register
    mov al, 0x80
    out dx, al                          ; Enable DLAB (set baud rate)
    mov dx, COM1                        ; Divisor Latch Low
    mov al, 0x01
    out dx, al                          ; 115200 baud (divisor = 1)
    mov dx, COM1 + 1                    ; Divisor Latch High
    xor al, al
    out dx, al                          ; High byte = 0
    mov dx, COM1 + 3                    ; Line Control Register
    mov al, 0x03
    out dx, al                          ; 8 bits, no parity, 1 stop (DLAB off)
    mov dx, COM1 + 2                    ; FIFO Control Register
    mov al, 0xC7
    out dx, al                          ; Enable FIFO, clear TX/RX, 14-byte threshold
    mov dx, COM1 + 4                    ; Modem Control Register
    mov al, 0x03
    out dx, al                          ; DTR + RTS

    ; ── Serial "K0\r\n"  -  _start reached, serial ready ──
    SERIAL_CHAR 'K'
    SERIAL_CHAR '0'
    SERIAL_CHAR 0x0D
    SERIAL_CHAR 0x0A

    ; ══════════════════════════════════════════════════════════════════════
    ;  PC SPEAKER BEEP  -  audible proof-of-life, works on all PCs.
    ;  No serial cable, no monitor, no framebuffer needed.
    ;  ~1000 Hz for ~200ms.
    ; ══════════════════════════════════════════════════════════════════════
    mov al, 0xB6                        ; PIT channel 2, LSB+MSB, mode 3
    out 0x43, al
    mov ax, 1193                        ; Divisor for ~1000 Hz
    out 0x42, al                        ; LSB
    mov al, ah
    out 0x42, al                        ; MSB
    in al, 0x61                         ; Read System Control Port B
    or al, 0x03                         ; Enable PIT ch2 gate + speaker
    out 0x61, al
    ; Short delay: ~200ms on a modern CPU
    mov ecx, 20000000
.beep_delay:
    dec ecx
    jnz .beep_delay
    in al, 0x61
    and al, 0xFC                        ; Disable speaker
    out 0x61, al

    ; ── EARLY DEBUG: write "K1" to VGA text buffer at 0xB8000 ──
    ;    (only visible on BIOS/CSM boots, invisible on UEFI)
    mov dword [0xB8000], 0x0F4B0F31    ; '1''K' white-on-black
    mov dword [0xB8004], 0x0F200F20    ; two spaces

    ; ══════════════════════════════════════════════════════════════════════
    ;  Ensure A20 gate is enabled.
    ; ══════════════════════════════════════════════════════════════════════
    in al, 0x92                         ; System Control Port A
    test al, 0x02                       ; A20 already enabled?
    jnz .a20_done
    or al, 0x02                         ; Set A20 enable bit
    and al, 0xFE                        ; Clear bit 0 (system reset!)
    out 0x92, al
.a20_done:

    ; ══════════════════════════════════════════════════════════════════════
    ;  EARLY FRAMEBUFFER TEST (32-bit, pre-paging)
    ;  Works for BOTH Multiboot1 AND Multiboot2.
    ;  On UEFI, VGA 0xB8000 is invisible  -  the framebuffer is the ONLY
    ;  way to get visible output.  This is critical for debugging.
    ; ══════════════════════════════════════════════════════════════════════

    ; ── Multiboot1 path: fixed-layout struct ──
    cmp esi, 0x2BADB002
    jne .try_mb2_fb
    mov eax, [edi]                      ; flags
    test eax, (1 << 12)                 ; bit 12 = framebuffer info present?
    jz  .no_early_fb
    mov eax, [edi + 92]                 ; framebuffer_addr upper 32 bits
    test eax, eax
    jnz .no_early_fb                    ; skip if above 4 GB
    mov ebx, [edi + 88]                 ; framebuffer_addr lower 32 bits
    test ebx, ebx
    jz  .no_early_fb
    jmp .do_early_fb_fill

    ; ── Multiboot2 path: parse tagged info structure for FB tag ──
.try_mb2_fb:
    cmp esi, 0x36d76289                 ; Multiboot2 magic?
    jne .no_early_fb
    ; MB2 info struct: [dword total_size] [dword reserved] [tags...]
    ; Each tag: [dword type] [dword size] [payload...], 8-byte aligned
    mov ebx, edi                        ; EBX = base of MB2 info
    mov ecx, [ebx]                      ; ECX = total_size
    add ecx, ebx                        ; ECX = end pointer
    add ebx, 8                          ; EBX = first tag
.mb2_tag_scan:
    cmp ebx, ecx
    jge .no_early_fb                    ; past end
    mov eax, [ebx]                      ; tag type
    cmp eax, 0                          ; end tag?
    je  .no_early_fb
    cmp eax, 8                          ; type 8 = framebuffer?
    je  .mb2_found_fb
    ; Advance to next tag: size = [ebx+4], aligned to 8
    mov eax, [ebx + 4]
    add eax, 7
    and eax, ~7
    add ebx, eax
    jmp .mb2_tag_scan
.mb2_found_fb:
    ; MB2 FB tag layout: +0 type, +4 size, +8 fb_addr(u64),
    ;   +16 pitch(u32), +20 width(u32), +24 height(u32), +28 bpp(u8)
    mov eax, [ebx + 12]                 ; fb_addr high 32 bits
    test eax, eax
    jnz .no_early_fb                    ; skip if above 4 GB
    mov ebx, [ebx + 8]                  ; fb_addr low 32 bits
    test ebx, ebx
    jz  .no_early_fb
    ; Fall through to fill

.do_early_fb_fill:
    ; EBX = framebuffer physical address (below 4 GB)
    ; Write bright-magenta pixels  -  ~2 scanlines at 1024×32bpp
    SERIAL_CHAR 'F'
    SERIAL_CHAR 'B'
    mov ecx, 8000
    mov eax, 0xFFFF00FF                 ; BGRA magenta
.early_fb_loop:
    mov [ebx], eax
    add ebx, 4
    dec ecx
    jnz .early_fb_loop
    wbinvd                              ; flush CPU caches → GPU VRAM
.no_early_fb:

    ; ══════════════════════════════════════════════════════════════════════
    ;  Verify CPU supports Long Mode via CPUID
    ; ══════════════════════════════════════════════════════════════════════
    pushfd
    pop eax
    mov ecx, eax
    xor eax, (1 << 21)                 ; Toggle EFLAGS.ID
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    xor eax, ecx
    jz .no_long_mode                    ; CPUID not supported

    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_long_mode

    mov eax, 0x80000001
    cpuid
    test edx, (1 << 29)                ; Long Mode bit
    jz .no_long_mode
    jmp .cpuid_ok

.no_long_mode:
    SERIAL_CHAR '!'
    SERIAL_CHAR 'L'
    SERIAL_CHAR 'M'
    SERIAL_CHAR 0x0D
    SERIAL_CHAR 0x0A
    cli
.no_lm_halt:
    hlt
    jmp .no_lm_halt

.cpuid_ok:
    ; ── Serial "K1\r\n"  -  pre-paging init complete ──
    SERIAL_CHAR 'K'
    SERIAL_CHAR '1'
    SERIAL_CHAR 0x0D
    SERIAL_CHAR 0x0A

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
    mov dword [pml4 + 4], 0

    ; ── PDPT[0..NUM_PDS-1] → PD tables ──
    lea ebx, [pd_tables]
    xor ecx, ecx
.setup_pdpt:
    mov eax, ebx
    or  eax, 0x03
    mov [pdpt + ecx * 8], eax
    mov dword [pdpt + ecx * 8 + 4], 0
    add ebx, 4096
    inc ecx
    cmp ecx, NUM_PDS
    jl  .setup_pdpt

    ; ── Fill PD entries: each maps a 2 MB page ──
    ;    0x83 = Present | Writable | PageSize-2MB
    lea ebx, [pd_tables]
    mov eax, 0x83
    xor edx, edx
    mov ecx, NUM_PDS * 512
.fill_pd:
    mov [ebx],     eax
    mov [ebx + 4], edx
    add ebx, 8
    add eax, 0x200000
    adc edx, 0
    dec ecx
    jnz .fill_pd

    ; ── Serial "K2\r\n"  -  page tables built ──
    SERIAL_CHAR 'K'
    SERIAL_CHAR '2'
    SERIAL_CHAR 0x0D
    SERIAL_CHAR 0x0A

    mov dword [0xB8000], 0x0F4B0F32    ; VGA "K2"

    ; ══════════════════════════════════════════════════════════════════════
    ;  Activate Long Mode
    ; ══════════════════════════════════════════════════════════════════════

    SERIAL_CHAR 'K'
    SERIAL_CHAR '3'
    SERIAL_CHAR 0x0D
    SERIAL_CHAR 0x0A

    ; ── Enable PAE (CR4 bit 5)  -  MUST come before CR3 per Intel SDM §9.8.5 ──
    mov eax, cr4
    or  eax, CR4_PAE
    mov cr4, eax

    ; ── Load PML4 into CR3 (after PAE is set) ──
    mov eax, pml4
    mov cr3, eax

    ; ── Enable Long Mode (IA32_EFER.LME) ──
    mov ecx, MSR_EFER
    rdmsr
    or  eax, EFER_LME
    wrmsr

    ; ── Enable Paging + Numeric Exception ──
    mov eax, cr0
    or  eax, CR0_PG | CR0_NE
    mov cr0, eax

    ; ── Load 64-bit GDT (32-bit pointer, base is <4 GB) ──
    lgdt [gdt64_ptr32]

    ; ── Far jump to 64-bit code segment (selector 0x08) ──
    jmp 0x08:long_mode_entry

; ═══════════════════════════════════════════════════════════════════════════
;  64-bit Entry Point  -  CPU is now in Long Mode
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

    ; ── Reload GDT with proper 64-bit base pointer ──
    lgdt [gdt64_ptr64]

    ; ── Enable SSE (required for x86-64 ABI) ──
    mov rax, cr0
    and ax, 0xFFFB                      ; clear CR0.EM (bit 2)
    or  ax, 0x0002                      ; set   CR0.MP (bit 1)
    mov cr0, rax
    mov rax, cr4
    or  ax, (1 << 9) | (1 << 10)       ; CR4.OSFXSR + OSXMMEXCPT
    mov cr4, rax

    ; ── Program PAT for Write-Combining support ──
    mov ecx, 0x277                      ; IA32_PAT MSR
    rdmsr
    and eax, 0xFFFF00FF                 ; clear PAT1 (bits 15:8)
    or  eax, 0x00000100                 ; PAT1 = WC (0x01)
    wrmsr

    ; ── Set up 64-bit stack ──
    mov rsp, stack_top

    ; ── Serial "K4\r\n" ──
    SERIAL_CHAR 'K'
    SERIAL_CHAR '4'
    SERIAL_CHAR 0x0D
    SERIAL_CHAR 0x0A

    mov dword [0xB8000], 0x0F4B0F33    ; VGA "K3"

    ; ── Save multiboot values before BSS clear clobbers RDI ──
    mov r8d, esi                        ; r8  = magic       (zero-extended)
    mov r9d, edi                        ; r9  = mb_info_ptr (zero-extended)

    ; ── Zero BSS (kernel_bss_start → kernel_bss_end only, NOT boot_tables) ──
    mov rdi, kernel_bss_start
    mov rcx, kernel_bss_end
    sub rcx, rdi
    shr rcx, 3
    xor rax, rax
    rep stosq

    ; ── Serial "K5\r\n" ──
    SERIAL_CHAR 'K'
    SERIAL_CHAR '5'
    SERIAL_CHAR 0x0D
    SERIAL_CHAR 0x0A

    mov dword [0xB8000], 0x0F4B0F34    ; VGA "K4"
    wbinvd

    ; ── Call kernel_main(magic, mb_addr)  -  System V AMD64 ABI ──
    mov rdi, r8                         ; arg1 = magic
    mov rsi, r9                         ; arg2 = mb_addr
    call kernel_main

    ; ── Halt forever if kernel returns ──
    cli
.hang:
    hlt
    jmp .hang

; ═══════════════════════════════════════════════════════════════════════════
;  64-bit EFI Entry Point  -  used on UEFI x86_64 when MB2 tags 7+9 present
;
;  GRUB stays in 64-bit long mode and jumps here directly, completely
;  bypassing the fragile relocator (64→32 mode switch) that crashes on
;  some real hardware.
;
;  Machine state at entry:
;    • 64-bit long mode, paging ON (EFI page tables)
;    • EAX = 0x36d76289 (MB2 magic)
;    • EBX = physical address of MB2 info structure (< 4 GB)
;    • EFI Boot Services still active (GRUB did NOT call ExitBootServices)
;    • Interrupts disabled (IF=0)
;
;  We build our own identity-mapped page tables (0 - 16 GB), load our GDT,
;  switch CR3, then join the normal long_mode_entry path.
; ═══════════════════════════════════════════════════════════════════════════
global _start_efi64
_start_efi64:
    cli
    cld

    ; ── Save multiboot registers (32-bit values, zero-extended) ──
    mov esi, eax                        ; ESI = magic 0x36d76289
    mov edi, ebx                        ; EDI = MB2 info pointer

    ; ── Set up our own stack (GRUB's EFI stack may vanish later) ──
    mov rsp, stack_top

    ; ── Disable NMI ──
    in al, 0x70
    or al, 0x80
    out 0x70, al
    in al, 0x71

    ; ── Initialise COM1 (115200 8N1)  -  same as 32-bit path ──
    mov dx, COM1 + 1
    xor al, al
    out dx, al
    mov dx, COM1 + 3
    mov al, 0x80
    out dx, al
    mov dx, COM1
    mov al, 0x01
    out dx, al
    mov dx, COM1 + 1
    xor al, al
    out dx, al
    mov dx, COM1 + 3
    mov al, 0x03
    out dx, al
    mov dx, COM1 + 2
    mov al, 0xC7
    out dx, al
    mov dx, COM1 + 4
    mov al, 0x03
    out dx, al

    ; ── Serial "K0\r\n" + "EFI64\r\n"  -  identify entry path ──
    SERIAL_CHAR 'K'
    SERIAL_CHAR '0'
    SERIAL_CHAR 0x0D
    SERIAL_CHAR 0x0A
    SERIAL_CHAR 'E'
    SERIAL_CHAR 'F'
    SERIAL_CHAR 'I'
    SERIAL_CHAR '6'
    SERIAL_CHAR '4'
    SERIAL_CHAR 0x0D
    SERIAL_CHAR 0x0A

    ; ── PC SPEAKER BEEP (~1 kHz, ~200 ms) ──
    mov al, 0xB6
    out 0x43, al
    mov ax, 1193
    out 0x42, al
    mov al, ah
    out 0x42, al
    in al, 0x61
    or al, 0x03
    out 0x61, al
    mov ecx, 20000000
.efi64_beep:
    dec ecx
    jnz .efi64_beep
    in al, 0x61
    and al, 0xFC
    out 0x61, al

    ; ── Early framebuffer: parse MB2 tags for FB, draw magenta pixels ──
    cmp esi, 0x36d76289
    jne .efi64_no_fb
    mov ebx, edi
    mov ecx, [rbx]                      ; total_size (64-bit addr via rbx)
    add ecx, ebx
    add ebx, 8
.efi64_tag_scan:
    cmp ebx, ecx
    jge .efi64_no_fb
    mov eax, [rbx]
    cmp eax, 0
    je  .efi64_no_fb
    cmp eax, 8                          ; framebuffer tag?
    je  .efi64_found_fb
    mov eax, [rbx + 4]
    add eax, 7
    and eax, ~7
    add ebx, eax
    jmp .efi64_tag_scan
.efi64_found_fb:
    mov eax, [rbx + 12]
    test eax, eax
    jnz .efi64_no_fb
    mov ebx, [rbx + 8]
    test ebx, ebx
    jz  .efi64_no_fb
    SERIAL_CHAR 'F'
    SERIAL_CHAR 'B'
    mov ecx, 8000
    mov eax, 0xFFFF00FF
.efi64_fb_loop:
    mov [rbx], eax
    add ebx, 4
    dec ecx
    jnz .efi64_fb_loop
    wbinvd
.efi64_no_fb:

    ; ══════════════════════════════════════════════════════════════════
    ;  Build our own identity-mapped page tables (0 → 16 GB)
    ;  Same structure as the 32-bit path.  We MUST replace EFI's page
    ;  tables because EFI's mappings are unknown and may not cover all
    ;  of our kernel's physical address space.
    ; ══════════════════════════════════════════════════════════════════

    ; ── Zero PML4 ──
    mov ecx, 4096 / 4
    xor eax, eax
    mov rbx, pml4
.efi64_zero_pml4:
    mov [rbx], eax
    add rbx, 4
    dec ecx
    jnz .efi64_zero_pml4

    ; ── Zero PDPT ──
    mov ecx, 4096 / 4
    mov rbx, pdpt
.efi64_zero_pdpt:
    mov [rbx], eax
    add rbx, 4
    dec ecx
    jnz .efi64_zero_pdpt

    ; ── PML4[0] → PDPT ──
    mov eax, pdpt
    or  eax, 0x03
    mov [pml4], eax
    mov dword [pml4 + 4], 0

    ; ── PDPT[0..NUM_PDS-1] → PD tables ──
    lea rbx, [pd_tables]
    xor ecx, ecx
.efi64_setup_pdpt:
    mov eax, ebx
    or  eax, 0x03
    mov [pdpt + ecx * 8], eax
    mov dword [pdpt + ecx * 8 + 4], 0
    add ebx, 4096
    inc ecx
    cmp ecx, NUM_PDS
    jl  .efi64_setup_pdpt

    ; ── Fill PD entries (2 MB pages) ──
    lea rbx, [pd_tables]
    mov eax, 0x83
    xor edx, edx
    mov ecx, NUM_PDS * 512
.efi64_fill_pd:
    mov [rbx],     eax
    mov [rbx + 4], edx
    add rbx, 8
    add eax, 0x200000
    adc edx, 0
    dec ecx
    jnz .efi64_fill_pd

    ; ── Extend the identity map to 2 TiB for high EFI GOP framebuffers ──
    ;    Some laptops place the GOP LFB above 512 GB (seen: ~1 TiB at
    ;    0xFA10000000), beyond the PD map above, so the kernel could not reach
    ;    the framebuffer to render. Map PML4[1..3] -> 3 PDPTs of 1 GiB pages
    ;    (no PDs needed), covering 512 GiB..2 TiB. PML4[0] is untouched so the
    ;    low map and the BIOS path are unchanged. 1 GiB pages need Page1GB
    ;    (standard on modern CPUs). (satoru)
    lea rbx, [efi_hi_pdpts]            ; base of the 3 hi PDPTs
    mov rax, pml4
    mov rcx, 1                          ; PML4 index 1..3
.efi64_hi_pml4:
    mov rdx, rbx
    or  rdx, 0x03                       ; present | write
    mov [rax + rcx*8], rdx              ; PML4[rcx] -> hi PDPT
    add rbx, 4096
    inc rcx
    cmp rcx, 4
    jl  .efi64_hi_pml4
    lea rbx, [efi_hi_pdpts]            ; fill 1536 entries (3 PDPTs)
    xor rcx, rcx
.efi64_hi_fill:
    mov rax, rcx
    add rax, 512                        ; phys index in GiB (this map starts at 512 GiB)
    shl rax, 30                         ; * 1 GiB
    or  rax, 0x83                       ; present | write | PS (1 GiB page)
    mov [rbx + rcx*8], rax
    inc rcx
    cmp rcx, 1536
    jl  .efi64_hi_fill

    SERIAL_CHAR 'K'
    SERIAL_CHAR '2'
    SERIAL_CHAR 0x0D
    SERIAL_CHAR 0x0A

    SERIAL_CHAR 'K'
    SERIAL_CHAR '3'
    SERIAL_CHAR 0x0D
    SERIAL_CHAR 0x0A

    ; ── Switch CR3 to our identity-mapped page tables ──
    ;    PAE + EFER.LME + CR0.PG are already set (we're in long mode).
    ;    Changing CR3 is safe because our tables identity-map 0 - 16 GB,
    ;    which includes the code we're executing (~0x100000).
    mov rax, pml4
    mov cr3, rax

    ; ── Load our GDT and reload CS via far return ──
    lgdt [gdt64_ptr64]

    push qword 0x08                     ; gdt64.code selector
    lea  rax, [rel .efi64_cs_done]
    push rax
    db   0x48                           ; REX.W prefix
    retf                                ; 64-bit far return → loads CS:RIP

.efi64_cs_done:
    ; ── Set data segments ──
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; ── ESI = magic, EDI = info pointer  -  preserved from the top ──
    ;    Jump into the shared 64-bit path for SSE, PAT, stack, BSS,
    ;    serial K4/K5, and kernel_main.
    jmp long_mode_entry
