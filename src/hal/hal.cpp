#include "hal.h"
#include "../drivers/serial.h"
#include "../linux/linux_syscall.h"
#include "../kernel/panic.h"
#include "../proc/scheduler.h"

//  x86_64 idt, pic 8259a, and isr implementation
//  isr stubs live in isr_stubs.asm  -  this file builds the idt from the
//  stub table and handles the c-level interrupt dispatch.

volatile uint64_t HAL::pit_ticks = 0;
volatile bool     HAL::irq_fired[16] = {};
HAL::IRQHandler   HAL::irq_handlers[16] = {};

namespace {
struct GDTDescriptor {
    uint64_t value;
} __attribute__((packed));

struct GDTPointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

struct TSS64 {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

constexpr int GDT_ENTRY_COUNT = 7;
alignas(16) GDTDescriptor gdt_entries[GDT_ENTRY_COUNT] = {};
GDTPointer gdt_ptr = {};
TSS64 system_tss = {};
alignas(16) uint8_t privilege_stack[16384] = {};
HAL::SystemCallHandler syscall_handler = nullptr;

static void BuildTSSDescriptor(uint64_t base, uint32_t limit) {
    uint64_t low = 0;
    low |= (uint64_t)(limit & 0xFFFF);
    low |= (base & 0xFFFFFFULL) << 16;
    low |= 0x89ULL << 40;
    low |= ((uint64_t)((limit >> 16) & 0xF)) << 48;
    low |= ((base >> 24) & 0xFFULL) << 56;

    gdt_entries[5].value = low;
    gdt_entries[6].value = base >> 32;
}
}

extern "C" {
    extern uint64_t isr_stub_table[48];   // 48 function pointers (vectors 0-47)
    extern void isr_stub_128();
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
    idt_entries[vec].selector    = GDT_KERNEL_CODE_SELECTOR;
    idt_entries[vec].ist         = ist & 0x07;
    idt_entries[vec].type_attr   = 0x80 | ((dpl & 3) << 5) | 0x0E;  // present + interrupt gate
    idt_entries[vec].offset_mid  = (uint16_t)((handler >> 16) & 0xFFFF);
    idt_entries[vec].offset_high = (uint32_t)((handler >> 32) & 0xFFFFFFFF);
    idt_entries[vec].reserved    = 0;
}

void HAL::InitGDT() {
    gdt_entries[0].value = 0;
    gdt_entries[1].value = 0x00AF9A000000FFFFULL;
    gdt_entries[2].value = 0x00CF92000000FFFFULL;
    gdt_entries[3].value = 0x00CFF2000000FFFFULL;
    gdt_entries[4].value = 0x00AFFA000000FFFFULL;

    for (size_t index = 0; index < sizeof(system_tss); index++) {
        ((uint8_t*)&system_tss)[index] = 0;
    }
    system_tss.rsp0 = (uint64_t)(uintptr_t)(privilege_stack + sizeof(privilege_stack));
    system_tss.ist1 = system_tss.rsp0;
    system_tss.iomap_base = sizeof(TSS64);
    BuildTSSDescriptor((uint64_t)(uintptr_t)&system_tss, sizeof(TSS64) - 1);

    gdt_ptr.limit = sizeof(gdt_entries) - 1;
    gdt_ptr.base = (uint64_t)(uintptr_t)&gdt_entries[0];

    asm volatile("lgdt %0" : : "m"(gdt_ptr) : "memory");

    uint16_t kernel_data = GDT_KERNEL_DATA_SELECTOR;
    asm volatile(
        "mov %0, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%ss\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        :
        : "rm"(kernel_data)
        : "ax", "memory"
    );

    uint16_t tss_selector = GDT_TSS_SELECTOR;
    asm volatile("ltr %0" : : "rm"(tss_selector) : "memory");
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

    if (vec == 0x80) {
        if (syscall_handler) {
            syscall_handler(frame);
        } else {
            frame->rax = (uint64_t)-1;
        }
        return;
    }

    if (vec < 32) {
        if (vec == 14) {
            // 1) Adaptive kernel-stack growth: if CR2 falls in any kernel
            //    process's guard page, allocate a fresh page and resume.
            //    Runs first so user-mode demand-zero handling never sees
            //    these faults.
            if (Scheduler::TryGrowGuardPage(frame->cr2)) {
                return;
            }
            // 2) User-mode page-fault dispatch (demand-zero, COW, etc.).
            if (LinuxSyscall::HandlePageFault(frame)) {
                return;
            }
        }

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
            // Drive the preemptive scheduler from the timer IRQ.  At
            // 1 kHz PIT this is one millisecond of charged runtime per
            // tick.  Scheduler::OnTimerTick() handles vruntime / sleep
            // wakeups / timeslice expiry; the actual switch happens at
            // the next voluntary Yield/Sleep so the IRQ stack stays
            // pristine.
            Scheduler::OnTimerTick(1);
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

    // user-mode syscall trap gate (int 0x80).
    idt_set(0x80, (uint64_t)(uintptr_t)&isr_stub_128, 0, 3);

    // vectors 48-255: leave as not-present (type_attr = 0).
    // if hardware triggers one, the cpu will fire a #gp which we handle above.

    // load the idt register
    idt_ptr.limit = sizeof(idt_entries) - 1;
    idt_ptr.base  = (uint64_t)&idt_entries;
    asm volatile("lidt %0" : : "m"(idt_ptr));
}

//  hal public api
void HAL::Init() {
    InitGDT();
    // 1. remap pic: irq 0-15 → vectors 32-47, all masked
    InitPIC();
    // 2. build and load idt from nasm stub table
    InitIDT();
    // 2.5 program SYSCALL/SYSRET MSRs (depends on GDT being loaded)
    InitSyscallMSRs();
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

    SerialLogger::Log("HAL: GDT/TSS loaded, IDT ready (IRQs + int 0x80), PIC remapped, IRQ 0/1/12 unmasked, NMI enabled\r\n");
}

void HAL::RegisterSystemCallHandler(SystemCallHandler handler) {
    syscall_handler = handler;
}

extern "C" void syscall_entry_x64();
extern "C" volatile uint64_t g_kernel_syscall_rsp;

void HAL::SetKernelStack(uint64_t rsp0) {
    if (!rsp0) return;
    system_tss.rsp0 = rsp0;
    system_tss.ist1 = rsp0;

    // Mirror to the global the SYSCALL entry stub reads.
    g_kernel_syscall_rsp = rsp0;
}

void HAL::InitSyscallMSRs() {
    // EFER (0xC0000080): bit 0 = SCE (System Call Extensions).
    // STAR (0xC0000081):
    //   bits[31:0]   = legacy 32-bit SYSCALL EIP (unused in long mode)
    //   bits[47:32]  = SYSCALL CS (kernel CS).  SS = CS+8 (kernel data).
    //   bits[63:48]  = SYSRET base.  CS = base+16 with RPL=3, SS = base+8.
    //
    // Our GDT layout:
    //   0x08 kernel code, 0x10 kernel data,
    //   0x18 user data,   0x20 user code.
    // → SYSCALL base = 0x08 (CS=0x08, SS=0x10) ✓
    // → SYSRET base  = 0x10 (user SS=0x18|3=0x1B, user CS=0x20|3=0x23) ✓
    //
    // LSTAR (0xC0000082): 64-bit syscall handler entry point.
    // SFMASK (0xC0000084): bits to *clear* from RFLAGS on entry.
    //   We clear IF (0x200) so syscalls run with interrupts disabled,
    //   plus DF (0x400) per SysV ABI, plus AC/TF for safety.

    constexpr uint32_t MSR_EFER     = 0xC0000080u;
    constexpr uint32_t MSR_STAR     = 0xC0000081u;
    constexpr uint32_t MSR_LSTAR    = 0xC0000082u;
    constexpr uint32_t MSR_SFMASK   = 0xC0000084u;

    auto rdmsr = [](uint32_t msr) -> uint64_t {
        uint32_t lo = 0, hi = 0;
        asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
        return ((uint64_t)hi << 32) | lo;
    };
    auto wrmsr = [](uint32_t msr, uint64_t value) {
        uint32_t lo = (uint32_t)(value & 0xFFFFFFFFu);
        uint32_t hi = (uint32_t)(value >> 32);
        asm volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(msr));
    };

    // Enable SCE.
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= 1ULL;
    wrmsr(MSR_EFER, efer);

    // Program STAR.
    uint64_t star = ((uint64_t)0x08ULL << 32) | ((uint64_t)0x10ULL << 48);
    wrmsr(MSR_STAR, star);

    // Program LSTAR with the asm entry point.
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)&syscall_entry_x64);

    // Program SFMASK: clear IF, DF, TF, AC, NT.
    wrmsr(MSR_SFMASK, 0x47700ULL);

    SerialLogger::Log("HAL: SYSCALL/SYSRET MSRs programmed (EFER.SCE, STAR, LSTAR, SFMASK)\r\n");
}

void HAL::EnableInterrupts()  { asm volatile("sti"); }
void HAL::DisableInterrupts() { asm volatile("cli"); }
void HAL::Halt()              { asm volatile("hlt"); }

void HAL::WaitForInterrupt() {
    asm volatile("sti; hlt");   // atomic enable + halt: wakes on any irq
}

[[noreturn]] void HAL::EnterUserMode(uint64_t rip, uint64_t rsp) {
    uint64_t rflags;
    asm volatile("pushfq; pop %0" : "=r"(rflags));
    rflags |= 0x202ULL;
    rflags &= ~0x3000ULL;  // keep IOPL at 0 in user mode

    uint16_t user_data = (uint16_t)(GDT_USER_DATA_SELECTOR | 3);
    uint64_t user_ss = (uint64_t)user_data;
    uint64_t user_cs = (uint64_t)(GDT_USER_CODE_SELECTOR | 3);

    asm volatile(
        "mov %0, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "pushq %1\n\t"
        "pushq %2\n\t"
        "pushq %3\n\t"
        "pushq %4\n\t"
        "pushq %5\n\t"
        "iretq\n\t"
        :
        : "rm"(user_data), "r"(user_ss), "r"(rsp), "r"(rflags), "r"(user_cs), "r"(rip)
        : "ax", "memory"
    );

    __builtin_unreachable();
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
