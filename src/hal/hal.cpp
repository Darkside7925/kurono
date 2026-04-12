#include "hal.h"
#include "../drivers/serial.h"
#include "../kernel/panic.h"

//  x86_64 idt, pic 8259a, and isr implementation
//  isr stubs live in isr_stubs.asm  -  this file builds the idt from the
//  stub table and handles the c-level interrupt dispatch.

volatile uint64_t HAL::pit_ticks = 0;
volatile bool     HAL::irq_fired[16] = {};
HAL::IRQHandler   HAL::irq_handlers[16] = {};

extern "C" {
    extern uint64_t isr_stub_table[48];   // 48 function pointers (vectors 0-47)
}

static const char* exception_names[32] = {
    "Divide-by-Zero (#DE)",           // 0
    "Debug (#DB)",                    // 1
    "Non-Maskable Interrupt (NMI)",   // 2
    "Breakpoint (#BP)",               // 3
    "Overflow (#OF)",                 // 4
    "Bound Range Exceeded (#BR)",     // 5
    "Invalid Opcode (#UD)",           // 6
    "Device Not Available (#NM)",     // 7
    "Double Fault (#DF)",             // 8
    "Coprocessor Segment Overrun",    // 9
    "Invalid TSS (#TS)",              // 10
    "Segment Not Present (#NP)",      // 11
    "Stack-Segment Fault (#SS)",      // 12
    "General Protection Fault (#GP)", // 13
    "Page Fault (#PF)",               // 14
    "Reserved",                       // 15
    "x87 FP Exception (#MF)",        // 16
    "Alignment Check (#AC)",          // 17
    "Machine Check (#MC)",            // 18
    "SIMD FP Exception (#XM)",       // 19
    "Virtualization (#VE)",           // 20
    "Control Protection (#CP)",       // 21
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved",
    "VMM Communication (#VC)",        // 29
    "Security Exception (#SX)",       // 30
    "Reserved"                        // 31
};

//  idt structures (x86_64: each entry is 16 bytes)
struct IDTEntry {
    uint16_t offset_low;    // bits 0-15 of handler address
    uint16_t selector;      // gdt code segment selector
    uint8_t  ist;           // interrupt stack table index (0 = none)
    uint8_t  type_attr;     // present | dpl | gate type
    uint16_t offset_mid;    // bits 16-31
    uint32_t offset_high;   // bits 32-63
    uint32_t reserved;
} __attribute__((packed));

struct IDTPointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static IDTEntry  idt_entries[256];
static IDTPointer idt_ptr;

static void idt_set(int vec, uint64_t handler, uint8_t ist = 0, uint8_t dpl = 0) {
    idt_entries[vec].offset_low  = (uint16_t)(handler & 0xFFFF);
    idt_entries[vec].selector    = 0x08;    // kernel code segment (gdt entry 1)
    idt_entries[vec].ist         = ist & 0x07;
    idt_entries[vec].type_attr   = 0x80 | ((dpl & 3) << 5) | 0x0E;  // present + interrupt gate
    idt_entries[vec].offset_mid  = (uint16_t)((handler >> 16) & 0xFFFF);
    idt_entries[vec].offset_high = (uint32_t)((handler >> 32) & 0xFFFFFFFF);
    idt_entries[vec].reserved    = 0;
}

//  pic 8259a

// pic port addresses
#define PIC1_CMD   0x20
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1
#define PIC_EOI    0x20

void HAL::InitPIC() {
    // icw1: begin initialization sequence (cascade mode, icw4 needed)
    OutByte(PIC1_CMD,  0x11);  IOWait();
    OutByte(PIC2_CMD,  0x11);  IOWait();

    // icw2: vector base offsets  -  irq0-7 → 32-39, irq8-15 → 40-47
    OutByte(PIC1_DATA, 0x20);  IOWait();
    OutByte(PIC2_DATA, 0x28);  IOWait();

    // icw3: master has slave on irq2 (bit 2), slave cascade identity = 2
    OutByte(PIC1_DATA, 0x04);  IOWait();
    OutByte(PIC2_DATA, 0x02);  IOWait();

    // icw4: 8086/88 mode, normal eoi
    OutByte(PIC1_DATA, 0x01);  IOWait();
    OutByte(PIC2_DATA, 0x01);  IOWait();

    // ocw1: mask all irqs initially  -  the kernel will selectively enable them
    OutByte(PIC1_DATA, 0xFF);
    OutByte(PIC2_DATA, 0xFF);
}

void HAL::EnableIRQ(uint8_t irq) {
    uint16_t port;
    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
        // unmask cascade (irq2) on master to receive slave interrupts
        uint8_t master = InByte(PIC1_DATA);
        OutByte(PIC1_DATA, master & ~(1 << 2));
    }
    uint8_t mask = InByte(port);
    OutByte(port, mask & ~(1 << irq));
}

void HAL::DisableIRQ(uint8_t irq) {
    uint16_t port;
    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    uint8_t mask = InByte(port);
    OutByte(port, mask | (1 << irq));
}

void HAL::SendEOI(uint8_t irq) {
    if (irq >= 8) {
        OutByte(PIC2_CMD, PIC_EOI);   // eoi to slave
    }
    OutByte(PIC1_CMD, PIC_EOI);       // eoi to master (always)
}

void HAL::RegisterIRQHandler(uint8_t irq, IRQHandler handler) {
    if (irq < 16) {
        irq_handlers[irq] = handler;
    }
}

//  common c interrupt handler  -  called from isr_stubs.asm
static void log_hex(const char* prefix, uint64_t val) {
    char buf[32];
    const char* hex = "0123456789ABCDEF";
    int len = 0;
    while (prefix[len]) { buf[len] = prefix[len]; len++; }
    buf[len++] = '0'; buf[len++] = 'x';
    for (int i = 60; i >= 0; i -= 4) {
        buf[len++] = hex[(val >> i) & 0xF];
    }
    buf[len++] = '\r'; buf[len++] = '\n'; buf[len] = 0;
    SerialLogger::Log(buf);
}

extern "C" void isr_common_handler(InterruptFrame* frame) {
    uint64_t vec = frame->vector;

    if (vec < 32) {
        SerialLogger::Log("\r\n!!! EXCEPTION: ");
        SerialLogger::Log(exception_names[vec]);
        SerialLogger::Log(" !!!\r\n");
        log_hex("  RIP    = ", frame->rip);
        log_hex("  CS     = ", frame->cs);
        log_hex("  RFLAGS = ", frame->rflags);
        log_hex("  RSP    = ", frame->rsp);
        log_hex("  ErrCode= ", frame->error_code);
        if (vec == 14) {
            log_hex("  CR2    = ", frame->cr2);
        }
        log_hex("  RAX    = ", frame->rax);
        log_hex("  RBX    = ", frame->rbx);
        log_hex("  RCX    = ", frame->rcx);
        log_hex("  RDX    = ", frame->rdx);
        log_hex("  RDI    = ", frame->rdi);
        log_hex("  RSI    = ", frame->rsi);
        log_hex("  RBP    = ", frame->rbp);

        SerialLogger::Log("Invoking kernel panic path.\r\n");
        KernelPanic::BugCheckFromInterrupt(frame, exception_names[vec]);
    }

    if (vec >= 32 && vec < 48) {
        uint8_t irq = (uint8_t)(vec - 32);

        // spurious irq detection: check pic isr register
        if (irq == 7) {
            // read master pic isr (ocw3: read isr)
            HAL::OutByte(PIC1_CMD, 0x0B);
            if (!(HAL::InByte(PIC1_CMD) & 0x80)) {
                return;  // spurious  -  no eoi
            }
        }
        if (irq == 15) {
            // read slave pic isr
            HAL::OutByte(PIC2_CMD, 0x0B);
            if (!(HAL::InByte(PIC2_CMD) & 0x80)) {
                // spurious from slave  -  still must eoi master
                HAL::OutByte(PIC1_CMD, PIC_EOI);
                return;
            }
        }

        // built-in tracking
        HAL::irq_fired[irq] = true;
        if (irq == 0) {
            HAL::pit_ticks++;
        }

        // call registered handler (if any)
        if (HAL::irq_handlers[irq]) {
            HAL::irq_handlers[irq](frame);
        }

        // send eoi
        HAL::SendEOI(irq);
        return;
    }

    log_hex("HAL: unhandled interrupt vector ", vec);
}

//  idt initialization  -  populate all 256 entries from the nasm stub table
void HAL::InitIDT() {
    // clear all entries
    for (int i = 0; i < 256; i++) {
        idt_entries[i] = {};
    }

    // vectors 0-47 come from isr_stub_table (built in isr_stubs.asm)
    for (int i = 0; i < 48; i++) {
        idt_set(i, isr_stub_table[i]);
    }

    // vectors 48-255: leave as not-present (type_attr = 0).
    // if hardware triggers one, the cpu will fire a #gp which we handle above.

    // load the idt register
    idt_ptr.limit = sizeof(idt_entries) - 1;
    idt_ptr.base  = (uint64_t)&idt_entries;
    asm volatile("lidt %0" : : "m"(idt_ptr));
}

//  hal public api
void HAL::Init() {
    // 1. remap pic: irq 0-15 → vectors 32-47, all masked
    InitPIC();
    // 2. build and load idt from nasm stub table
    InitIDT();
    // 3. selectively enable interrupts we actually use
    EnableIRQ(0);   // pit timer
    EnableIRQ(1);   // keyboard
    EnableIRQ(12);  // mouse

    // 4. re-enable nmi now that idt is ready to handle it.
    //    boot assembly disables nmi via cmos port 0x70 bit 7 to prevent
    //    triple-faults before the idt exists.
    uint8_t cmos = InByte(0x70);
    OutByte(0x70, cmos & 0x7F);   // clear bit 7 = nmi enable
    InByte(0x71);                  // dummy read completes cmos cycle

    SerialLogger::Log("HAL: IDT loaded (48 NASM stubs), PIC remapped, IRQ 0/1/12 unmasked, NMI enabled\r\n");
}

void HAL::EnableInterrupts()  { asm volatile("sti"); }
void HAL::DisableInterrupts() { asm volatile("cli"); }
void HAL::Halt()              { asm volatile("hlt"); }

void HAL::WaitForInterrupt() {
    asm volatile("sti; hlt");   // atomic enable + halt: wakes on any irq
}

void HAL::Reboot() {
    DisableInterrupts();

    SerialLogger::Log("HAL: Reboot requested\r\n");
    while (true) {
        // 1. classic keyboard controller cpu reset pulse.
        for (int tries = 0; tries < 8; tries++) {
            uint32_t spin = 0;
            while ((InByte(0x64) & 0x02) && spin < 100000) spin++;
            OutByte(0x64, 0xFE);
            for (volatile uint32_t d = 0; d < 200000; d++) asm volatile("pause");
        }

        // 2. pci/reset control port. very common on modern chipsets.
        //    0x02 = hard reset request, 0x06 = full reset.
        OutByte(0xCF9, 0x02);
        for (volatile uint32_t d = 0; d < 200000; d++) asm volatile("pause");
        OutByte(0xCF9, 0x06);
        for (volatile uint32_t d = 0; d < 1000000; d++) asm volatile("pause");

        // 3. fast reset via system control port a.
        uint8_t port92 = InByte(0x92);
        OutByte(0x92, (uint8_t)(port92 | 0x01));
        for (volatile uint32_t d = 0; d < 1000000; d++) asm volatile("pause");

        // 4. triple-fault fallback: load a null idt and trigger exceptions.
        struct {
            uint16_t limit;
            uint64_t base;
        } __attribute__((packed)) null_idt = {0, 0};
        asm volatile("lidt %0" : : "m"(null_idt));
        asm volatile("int3");
        asm volatile("ud2");
        asm volatile("cli; hlt");
    }
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

void HAL::IOWait() {
    // write to unused port 0x80  -  introduces ~1 µs delay for pic/slow devices
    asm volatile("outb %%al, $0x80" : : "a"((uint8_t)0));
}

void HAL::MemoryBarrier() {
    asm volatile("mfence" : : : "memory");
}
