#include "hal.h"
#include "../drivers/serial.h"

// ═══════════════════════════════════════════════════════════════════════════
//  x86_64 IDT, PIC, and ISR Implementation
// ═══════════════════════════════════════════════════════════════════════════

volatile uint64_t HAL::pit_ticks = 0;
volatile bool     HAL::irq_fired[16] = {};

// ── IDT structures (x86_64: 16 bytes per entry) ─────────────────────────
struct IDTEntry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;   // 0x8E = present, DPL=0, 64-bit interrupt gate
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

struct IDTPointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static IDTEntry idt_entries[256];
static IDTPointer idt_ptr;

static void idt_set(int vec, uint64_t handler) {
    idt_entries[vec].offset_low  = (uint16_t)(handler & 0xFFFF);
    idt_entries[vec].selector    = 0x08;   // kernel code segment
    idt_entries[vec].ist         = 0;
    idt_entries[vec].type_attr   = 0x8E;   // present, interrupt gate
    idt_entries[vec].offset_mid  = (uint16_t)((handler >> 16) & 0xFFFF);
    idt_entries[vec].offset_high = (uint32_t)((handler >> 32) & 0xFFFFFFFF);
    idt_entries[vec].reserved    = 0;
}

// ── PIC (8259A) remapping ────────────────────────────────────────────────
static void pic_remap() {
    // Save masks
    uint8_t m1 = HAL::InByte(0x21);
    uint8_t m2 = HAL::InByte(0xA1);

    // ICW1: init + ICW4 needed
    HAL::OutByte(0x20, 0x11);
    HAL::OutByte(0xA0, 0x11);
    // ICW2: vector offsets (IRQ0→32, IRQ8→40)
    HAL::OutByte(0x21, 0x20);   // master offset = 32
    HAL::OutByte(0xA1, 0x28);   // slave  offset = 40
    // ICW3: master has slave on IRQ2, slave cascade identity = 2
    HAL::OutByte(0x21, 0x04);
    HAL::OutByte(0xA1, 0x02);
    // ICW4: 8086 mode
    HAL::OutByte(0x21, 0x01);
    HAL::OutByte(0xA1, 0x01);

    // Mask ALL IRQs — kernel is fully polling-based (PIT counter,
    // keyboard/mouse I/O ports), so hardware interrupts are not needed.
    // This avoids WHPX VM-exit storms and interrupt delivery issues.
    HAL::OutByte(0x21, 0xFF);
    HAL::OutByte(0xA1, 0xFF);

    (void)m1; (void)m2;
}

// ── ISR stubs (AT&T syntax, defined at file scope) ──────────────────────
// These save caller-saved registers, call C handler, send EOI, and iretq.

// Forward declarations for C handlers
extern "C" void isr_timer_c(void);
extern "C" void isr_keyboard_c(void);
extern "C" void isr_mouse_c(void);
extern "C" void isr_spurious_c(void);
extern "C" void isr_default_c(void);

// Assembly ISR stubs
extern "C" void isr_stub_timer(void);
extern "C" void isr_stub_keyboard(void);
extern "C" void isr_stub_mouse(void);
extern "C" void isr_stub_spurious(void);
extern "C" void isr_stub_default(void);
extern "C" void isr_stub_exception(void);

// Timer IRQ0 (vector 32)
__asm__(
    ".global isr_stub_timer\n"
    "isr_stub_timer:\n"
    "    push %rax; push %rcx; push %rdx; push %rdi; push %rsi\n"
    "    push %r8;  push %r9;  push %r10; push %r11\n"
    "    call isr_timer_c\n"
    "    movb $0x20, %al\n"
    "    outb %al, $0x20\n"
    "    pop %r11; pop %r10; pop %r9;  pop %r8\n"
    "    pop %rsi; pop %rdi; pop %rdx; pop %rcx; pop %rax\n"
    "    iretq\n"
);

// Keyboard IRQ1 (vector 33)
__asm__(
    ".global isr_stub_keyboard\n"
    "isr_stub_keyboard:\n"
    "    push %rax; push %rcx; push %rdx; push %rdi; push %rsi\n"
    "    push %r8;  push %r9;  push %r10; push %r11\n"
    "    call isr_keyboard_c\n"
    "    movb $0x20, %al\n"
    "    outb %al, $0x20\n"
    "    pop %r11; pop %r10; pop %r9;  pop %r8\n"
    "    pop %rsi; pop %rdi; pop %rdx; pop %rcx; pop %rax\n"
    "    iretq\n"
);

// Mouse IRQ12 (vector 44) — need EOI to both PICs
__asm__(
    ".global isr_stub_mouse\n"
    "isr_stub_mouse:\n"
    "    push %rax; push %rcx; push %rdx; push %rdi; push %rsi\n"
    "    push %r8;  push %r9;  push %r10; push %r11\n"
    "    call isr_mouse_c\n"
    "    movb $0x20, %al\n"
    "    outb %al, $0xA0\n"    // EOI to slave PIC
    "    outb %al, $0x20\n"    // EOI to master PIC
    "    pop %r11; pop %r10; pop %r9;  pop %r8\n"
    "    pop %rsi; pop %rdi; pop %rdx; pop %rcx; pop %rax\n"
    "    iretq\n"
);

// Spurious IRQ7 / IRQ15 (just EOI)
__asm__(
    ".global isr_stub_spurious\n"
    "isr_stub_spurious:\n"
    "    push %rax\n"
    "    movb $0x20, %al\n"
    "    outb %al, $0x20\n"
    "    pop %rax\n"
    "    iretq\n"
);

// Default handler for other IRQs (send EOI to both PICs)
__asm__(
    ".global isr_stub_default\n"
    "isr_stub_default:\n"
    "    push %rax\n"
    "    movb $0x20, %al\n"
    "    outb %al, $0xA0\n"
    "    outb %al, $0x20\n"
    "    pop %rax\n"
    "    iretq\n"
);

// Exception handlers (vectors 0-31)
// Some exceptions push an error code (8, 10-14, 17, 21, 29, 30).
// We need separate stubs so iretq reads the correct return frame.
__asm__(
    ".global isr_stub_exc_noerr\n"
    "isr_stub_exc_noerr:\n"
    "    iretq\n"
);
__asm__(
    ".global isr_stub_exc_err\n"
    "isr_stub_exc_err:\n"
    "    add $8, %rsp\n"        // pop error code
    "    iretq\n"
);
extern "C" void isr_stub_exc_noerr(void);
extern "C" void isr_stub_exc_err(void);

// ── C interrupt handlers ─────────────────────────────────────────────────
extern "C" void isr_timer_c(void) {
    HAL::pit_ticks++;
    HAL::irq_fired[0] = true;
}

extern "C" void isr_keyboard_c(void) {
    HAL::irq_fired[1] = true;
}

extern "C" void isr_mouse_c(void) {
    HAL::irq_fired[12] = true;
}

extern "C" void isr_spurious_c(void) { }
extern "C" void isr_default_c(void)  { }

// ── IDT initialization ──────────────────────────────────────────────────
void HAL::InitIDT() {
    // Exceptions without error code: 0-7, 9, 15, 16, 18-20, 22-31
    for (int i = 0; i < 256; i++) {
        idt_set(i, (uint64_t)isr_stub_exc_noerr);
    }
    // Exceptions WITH error code: 8(DF), 10(TS), 11(NP), 12(SS), 13(GP), 14(PF), 17(AC), 21(CP), 29(VC), 30(SX)
    static const int err_vecs[] = {8, 10, 11, 12, 13, 14, 17, 21, 29, 30};
    for (int v : err_vecs) {
        idt_set(v, (uint64_t)isr_stub_exc_err);
    }

    // Install IRQ handlers (vectors 32-47)
    idt_set(32, (uint64_t)isr_stub_timer);       // IRQ0  — PIT
    idt_set(33, (uint64_t)isr_stub_keyboard);     // IRQ1  — Keyboard
    idt_set(34, (uint64_t)isr_stub_default);      // IRQ2  — Cascade
    idt_set(35, (uint64_t)isr_stub_default);      // IRQ3
    idt_set(36, (uint64_t)isr_stub_default);      // IRQ4
    idt_set(37, (uint64_t)isr_stub_default);      // IRQ5
    idt_set(38, (uint64_t)isr_stub_default);      // IRQ6
    idt_set(39, (uint64_t)isr_stub_spurious);     // IRQ7  — spurious
    idt_set(40, (uint64_t)isr_stub_default);      // IRQ8
    idt_set(41, (uint64_t)isr_stub_default);      // IRQ9
    idt_set(42, (uint64_t)isr_stub_default);      // IRQ10
    idt_set(43, (uint64_t)isr_stub_default);      // IRQ11
    idt_set(44, (uint64_t)isr_stub_mouse);        // IRQ12 — Mouse
    idt_set(45, (uint64_t)isr_stub_default);      // IRQ13
    idt_set(46, (uint64_t)isr_stub_default);      // IRQ14
    idt_set(47, (uint64_t)isr_stub_spurious);     // IRQ15 — spurious

    // Load IDT register
    idt_ptr.limit = sizeof(idt_entries) - 1;
    idt_ptr.base  = (uint64_t)&idt_entries;
    __asm__ __volatile__("lidt %0" : : "m"(idt_ptr));
}

// ═══════════════════════════════════════════════════════════════════════════
//  HAL public API
// ═══════════════════════════════════════════════════════════════════════════
void HAL::Init() {
    // Remap PIC: IRQ 0-15 → vectors 32-47
    pic_remap();
    // Build and load IDT
    InitIDT();
    // Interrupts still disabled — kernel_main will enable them
    SerialLogger::Log("HAL: IDT + PIC initialized\r\n");
}

void HAL::EnableInterrupts()  { asm volatile("sti"); }
void HAL::DisableInterrupts() { asm volatile("cli"); }
void HAL::Halt()              { asm volatile("hlt"); }

void HAL::WaitForInterrupt() {
    asm volatile("sti; hlt");
}

void HAL::Reboot() {
    uint8_t good = 0x02;
    while (good & 0x02) good = InByte(0x64);
    OutByte(0x64, 0xFE);
    Halt();
}

void HAL::OutByte(uint16_t port, uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}
uint8_t HAL::InByte(uint16_t port) {
    uint8_t value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
void HAL::OutWord(uint16_t port, uint16_t value) {
    asm volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}
uint16_t HAL::InWord(uint16_t port) {
    uint16_t value;
    asm volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
void HAL::OutLong(uint16_t port, uint32_t value) {
    asm volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}
uint32_t HAL::InLong(uint16_t port) {
    uint32_t value;
    asm volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
void HAL::MemoryBarrier() {
    asm volatile("" : : : "memory");
}
