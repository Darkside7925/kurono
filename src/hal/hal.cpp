#include "hal.h"
#include "../drivers/serial.h"
#include "../proc/smp.h"          // per-cpu kernel-stack (gs:8) for the syscall path (satoru)
#include "../proc/spinlock.h"     // serialize cross-core exception dumps (satoru)
#include "../linux/linux_syscall.h"
#include "../kernel/panic.h"
#include "../proc/scheduler.h"
#include "../kernel/userspace.h"   // Userspace::HandleProcessExit for user-fault termination (satoru)
#include "../kernel/vmm.h"         // resolve user va->phys to dump the tls/tcb at a fault (satoru)
#include "../kernel/kdf.h"         // kdf guard-page fault isolation (catches driver oob -> no panic) (satoru)

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
    extern void isr_stub_64();            // per-AP LAPIC timer (smp phase 4) (satoru)
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

//  per-cpu gdt + tss for the application processors. each ap needs its own tss:
//  rsp0 is loaded by the cpu on a ring3->ring0 transition (fault / int 0x80), so
//  two cores sharing one tss would corrupt each other's kernel entry stack. the
//  fixed code/data descriptors are copied from the bsp gdt; only the tss
//  descriptor differs per core. (satoru)
alignas(16) static GDTDescriptor ap_gdt[SMP_MAX_CPUS][GDT_ENTRY_COUNT];
static TSS64 ap_tss[SMP_MAX_CPUS];
alignas(16) static uint8_t ap_priv_stack[SMP_MAX_CPUS][16384];

void HAL::SetupAPCpuState() {
    uint32_t cpu = SMP::CpuIndex();
    if (cpu == 0 || cpu >= SMP_MAX_CPUS) return;   // bsp already set up via InitGDT (satoru)

    for (int i = 0; i < 5; i++) ap_gdt[cpu][i].value = gdt_entries[i].value;

    TSS64* t = &ap_tss[cpu];
    for (size_t i = 0; i < sizeof(TSS64); i++) ((uint8_t*)t)[i] = 0;
    t->rsp0 = (uint64_t)(uintptr_t)(ap_priv_stack[cpu] + sizeof(ap_priv_stack[cpu]));
    t->ist1 = t->rsp0;
    t->iomap_base = sizeof(TSS64);

    // 16-byte tss descriptor into gdt slots 5,6 (same layout as BuildTSSDescriptor). (satoru)
    uint64_t base = (uint64_t)(uintptr_t)t;
    uint32_t limit = sizeof(TSS64) - 1;
    uint64_t low = (uint64_t)(limit & 0xFFFF);
    low |= (base & 0xFFFFFFULL) << 16;
    low |= 0x89ULL << 40;
    low |= ((uint64_t)((limit >> 16) & 0xF)) << 48;
    low |= ((base >> 24) & 0xFFULL) << 56;
    ap_gdt[cpu][5].value = low;
    ap_gdt[cpu][6].value = base >> 32;

    GDTPointer gp;
    gp.limit = sizeof(GDTDescriptor) * GDT_ENTRY_COUNT - 1;
    gp.base  = (uint64_t)(uintptr_t)&ap_gdt[cpu][0];
    asm volatile("lgdt %0" : : "m"(gp) : "memory");

    uint16_t kd = GDT_KERNEL_DATA_SELECTOR;
    asm volatile(
        "mov %0, %%ax\n\t"
        "mov %%ax, %%ds\n\t mov %%ax, %%es\n\t mov %%ax, %%ss\n\t"
        "mov %%ax, %%fs\n\t mov %%ax, %%gs\n\t"
        : : "rm"(kd) : "ax", "memory");
    uint16_t ts = GDT_TSS_SELECTOR;
    asm volatile("ltr %0" : : "rm"(ts) : "memory");

    // share the bsp's idt (read-only after setup) so faults on this ap dispatch. (satoru)
    asm volatile("lidt %0" : : "m"(idt_ptr) : "memory");

    // per-core SYSCALL msrs (lstar/star/sfmask/efer.sce+nxe). (satoru)
    InitSyscallMSRs();
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

    // per-AP LAPIC timer (smp phase 4): preempt the user thread this ap is running,
    // then signal end-of-interrupt to the local apic. fires only while ring-3 user
    // code runs (ApTimerPreempt no-ops otherwise). (satoru)
    if (vec == 0x40) {
        Scheduler::ApTimerPreempt(frame);
        SMP::LapicWrite(0xB0, 0);   // lapic EOI register (satoru)
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
            // 1.5) KDF driver guard-page isolation: if CR2 lands in a kdf
            //      driver's fenced dma/mmio window (a guard page or a quarantined
            //      region), the kdf hook quarantines the region, dumps regs, logs,
            //      reports the crash to kinit, and longjmps the faulting driver
            //      operation back to its RunGuarded() call-site (it DOES NOT
            //      return in that case). when there is no armed sandbox to unwind
            //      to it returns true after quarantining, so we still avoid the
            //      kernel panic. only kdf addresses are claimed; everything else
            //      falls through to the normal user/kernel fault path below.
            //      this is the load-bearing "a driver crash doesn't take down the
            //      kernel" path of the hybrid architecture. (satoru)
            if ((frame->cs & 3) == 0 && KDF::HandleGuardFault(frame->cr2, frame->rip)) {
                return;
            }
            // 2) User-mode page-fault dispatch (demand-zero, COW, etc.).
            //
            //    A ring-3 fault can be a RESTARTABLE faulting SSE store: e.g.
            //    musl's memcpy fast path `movups %xmm0,(mem)` faulting on the
            //    first byte of a fresh demand-zero page. After we map the page
            //    and IRET, the cpu RE-EXECUTES that store from %xmm0  -  so %xmm0
            //    (the whole fpu/sse state) must be byte-identical to what the
            //    faulting code held. But the #pf path runs kernel code that
            //    touches xmm (memcpy/graphics inline asm, an IRQ-driven task
            //    switch's fxrstor), clobbering %xmm0. That silently zeroed the
            //    first 16 bytes of the page the restarted movups wrote  -  exactly
            //    the lost atom[0x50]/[0x51] pointers that #pf'd xkbcommon. fix:
            //    fxsave the user fpu/sse before handling and fxrstor it right
            //    before returning to ring-3, mirroring the SYSCALL fast path.
            //    skip the restore if the handler switched tasks (the new task's
            //    state was already loaded). (satoru)
            if ((frame->cs & 3) == 3) {
                // fxsave/fxrstor require a 16-byte-aligned operand. the ISR stub
                // does not guarantee the SysV stack alignment the compiler assumes
                // for an alignas(16) stack object (the hand-written stub leaves
                // rsp 16-aligned at the call, not the ABI rsp%16==8), so align the
                // pointer at runtime instead of trusting alignas. (satoru)
                uint8_t fxraw[512 + 16];
                uint8_t* fx = (uint8_t*)(((uintptr_t)fxraw + 15) & ~(uintptr_t)15);
                __asm__ __volatile__("fxsave (%0)" :: "r"(fx) : "memory");
                void* before = (void*)Scheduler::GetCurrentProcess();
                bool handled = LinuxSyscall::HandlePageFault(frame);
                if (handled) {
                    void* after = (void*)Scheduler::GetCurrentProcess();
                    if (after == before) {
                        __asm__ __volatile__("fxrstor (%0)" :: "r"(fx) : "memory");
                    }
                    return;
                }
            } else {
                if (LinuxSyscall::HandlePageFault(frame)) {
                    return;
                }
            }
        }

        // serialize the whole dump so a fault on an application processor isn't
        // interleaved char-by-char with bsp serial output. unlocked before
        // HandleProcessExit (which longjmps and would never release it). user
        // ring-3 code can't be holding this lock, so no deadlock. (satoru)
        static Spinlock g_exc_dump_lock;
        uint64_t _exc_f; g_exc_dump_lock.LockIrqSave(&_exc_f);
        SerialLogger::Log("\r\n!!! EXCEPTION: ");
        SerialLogger::Log(exception_names[vec]);
        SerialLogger::Log(" !!! cpu");
        SerialLogger::LogDec((int)SMP::CpuIndex());
        SerialLogger::Log("\r\n");
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
        log_hex("  R14    = ", frame->r14);
        log_hex("  R15    = ", frame->r15);
        // tls diagnostic for the firefox pthread_create #gp bring-up: dump the
        // live fs base, the saved proc->fs_base, and the tcb head words (self@0,
        // prev@0x10, next@0x18) so we can see empirically whether the live thread
        // pointer matches what musl __init_tp initialized. ring-3 only. (satoru)
        if ((frame->cs & 3) == 3) {
            uint32_t fsl, fsh;
            __asm__ __volatile__("rdmsr" : "=a"(fsl), "=d"(fsh) : "c"(0xC0000100));
            uint64_t live_fs = ((uint64_t)fsh << 32) | fsl;
            log_hex("  liveFS = ", live_fs);
            Process* fp = Scheduler::GetCurrentProcess();
            if (fp) {
                log_hex("  procFS = ", fp->fs_base);
                log_hex("  pid    = ", (uint64_t)fp->pid);
            }
            // read tcb words at the live fs base (and at r14 if it differs) by
            // resolving the user va -> phys in the faulting proc's address space.
            // r14 = musl's `self` (the thread descriptor it derived). (satoru)
            auto dump_tcb = [&](const char* tag, uint64_t va) {
                if (!fp || !va) return;
                for (uint64_t off = 0; off <= 0x18; off += 8) {
                    uint64_t pg = (va + off) & ~0xFFFULL;
                    uint64_t ph = KernelVMM::QueryMappingInAddressSpace(fp->address_space, pg);
                    char p[40]; int n = 0;
                    while (tag[n] && n < 8) { p[n] = tag[n]; n++; }
                    while (n < 8) p[n++] = ' ';
                    p[n++] = '+'; const char* hx = "0123456789ABCDEF";
                    p[n++] = hx[(off >> 4) & 0xF]; p[n++] = hx[off & 0xF];
                    p[n] = 0;
                    uint64_t v = ph ? *(volatile uint64_t*)(uintptr_t)(ph + ((va + off) & 0xFFF)) : 0xDEADULL;
                    log_hex(p, v);
                }
            };
            dump_tcb("liveTCB", live_fs);
            if (frame->r14 != live_fs) dump_tcb("r14TCB", frame->r14);
        }
        g_exc_dump_lock.UnlockIrqRestore(_exc_f);

        // a genuine ring-3 exception (segfault, #GP, #UD, div0, ...) must not
        // panic the whole kernel. terminate the faulting user process with
        // SIGSEGV semantics instead: HandleProcessExit marks it exited and
        // longjmps back to RunProcessWithArgs on the kernel stack. it no-ops
        // when there is no active user process, so a true kernel fault below
        // still panics as before. (satoru -- task #24)
        if ((frame->cs & 3) == 3) {
            SerialLogger::Log("Terminating faulting user process (SIGSEGV).\r\n");
            Userspace::HandleProcessExit(139);  // 128 + SIGSEGV; does not return
        }
        // kernel-mode fault diagnostic: dump cr3 + walk the page tables for cr2
        // so we can see whether the identity (or user) mapping is present in the
        // ACTIVE address space at the fault. helps pin the firefox pmm/teardown
        // #pf (a kernel write to a frame whose mapping is missing). (satoru)
        if (vec == 14 && (frame->cs & 3) == 0) {
            uint64_t cr3;
            __asm__ __volatile__("movq %%cr3, %0" : "=r"(cr3));
            log_hex("  CR3    = ", cr3);
            Process* kf = Scheduler::GetCurrentProcess();
            if (kf) { log_hex("  curAS  = ", kf->address_space); log_hex("  curPID = ", (uint64_t)kf->pid); }
            uint64_t va = frame->cr2;
            uint64_t* pml4 = (uint64_t*)(uintptr_t)(cr3 & ~0xFFFULL);
            uint64_t e4 = pml4[(va >> 39) & 0x1FF];
            log_hex("  pml4e  = ", e4);
            if (e4 & 1) {
                uint64_t* pdpt = (uint64_t*)(uintptr_t)(e4 & ~0xFFFULL & ~(1ULL<<63));
                uint64_t e3 = pdpt[(va >> 30) & 0x1FF];
                log_hex("  pdpte  = ", e3);
                if ((e3 & 1) && !(e3 & 0x80)) {
                    uint64_t* pd = (uint64_t*)(uintptr_t)(e3 & ~0xFFFULL & ~(1ULL<<63));
                    uint64_t e2 = pd[(va >> 21) & 0x1FF];
                    log_hex("  pde    = ", e2);
                    if ((e2 & 1) && !(e2 & 0x80)) {
                        uint64_t* pt = (uint64_t*)(uintptr_t)(e2 & ~0xFFFULL & ~(1ULL<<63));
                        log_hex("  pte    = ", pt[(va >> 12) & 0x1FF]);
                    }
                }
            }
        }
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
            // Drive the scheduler from the 1 kHz PIT: 1 ms of charged runtime
            // per tick. A fine tick keeps SleepMs wakeups (input/cursor) timely
            // for smooth 60fps; sleep deadlines + the ms clock are TSC-based and
            // independent of this rate. (satoru)
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

    // per-AP LAPIC timer (vector 0x40), kernel-only (dpl 0). (satoru)
    idt_set(0x40, (uint64_t)(uintptr_t)&isr_stub_64, 0, 0);

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
    // 3. enable the interrupts we use. keyboard (IRQ1) and mouse (IRQ12) are
    // POLLED (no handlers)  -  BUT we keep them UNMASKED on purpose: when the cpu
    // is HLT'd in deep idle, a keystroke/mouse-move IRQ wakes it immediately so
    // the polled InputProcess runs and the desktop responds without waiting for
    // the (WHPX-coalesced, ~500ms) timer IRQ. the unhandled IRQ just EOIs; the
    // wake is the point. (satoru)
    EnableIRQ(0);   // pit timer (drives scheduler wakeups)
    EnableIRQ(1);   // keyboard  -  polled, but wakes the cpu from HLT
    EnableIRQ(12);  // mouse  -  polled, but wakes the cpu from HLT

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

    // mirror to the legacy global (still defined) AND to this cpu's PerCpu block,
    // which the reworked SYSCALL stub reads via gs:8 after swapgs. SMP::Current()
    // resolves the calling cpu (reads the lapic) so this is correct on the bsp and
    // on any ap that runs the scheduler. (satoru)
    g_kernel_syscall_rsp = rsp0;
    SMP::Current()->kernel_rsp = rsp0;
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

    // Enable SCE (syscall) + NXE (no-execute enable). NXE is mandatory here:
    // our user brk/mmap demand-zero regions map with PTE_NX (bit 63). without
    // efer.nxe that bit is *reserved*, so every demand-paged heap/anon page
    // raises a reserved-bit #PF (err bit3). the fault handler remaps with the
    // same flag and the instruction re-faults forever, leaking a frame each
    // iteration until the pmm is exhausted and the kernel panics. enabling nxe
    // makes the bit mean no-execute (valid) and gives real w^x for user
    // heap/stack/anon memory. only mappings that explicitly set bit 63 are
    // affected; kernel code + elf segments never do. (satoru)
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= 1ULL;          // SCE
    efer |= (1ULL << 11);  // NXE
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

void HAL::PowerOff() {
    DisableInterrupts();
    SerialLogger::Log("HAL: PowerOff requested\r\n");

    // try the common emulator/acpi poweroff ports in order. each is a
    // 16-bit write of the sleep value to a hardware-reduced acpi pm1a-style
    // control port that the hypervisor watches. on real hardware these
    // usually do nothing, so we fall through to a halt. (satoru)
    for (int attempt = 0; attempt < 4; attempt++) {
        // qemu (>= 2.0) acpi pm control register.
        OutWord(0x604, 0x2000);
        for (volatile uint32_t d = 0; d < 500000; d++) asm volatile("pause");
        // bochs / older qemu (pre-2.0) acpi poweroff.
        OutWord(0xB004, 0x2000);
        for (volatile uint32_t d = 0; d < 500000; d++) asm volatile("pause");
        // cloud-hypervisor / firecracker acpi shutdown port.
        OutWord(0x4004, 0x3400);
        for (volatile uint32_t d = 0; d < 500000; d++) asm volatile("pause");
    }

    // none of the poweroff ports took effect (likely real hardware without
    // a parsed acpi fadt). leave the machine in a safe halted state. (satoru)
    SerialLogger::Log("HAL: PowerOff ports had no effect  -  halting CPU\r\n");
    while (true) {
        asm volatile("cli; hlt");
    }
}

namespace {
//  minimal acpi fadt locator (satoru)
//  walks rsdp -> rsdt/xsdt -> fadt to recover pm1a_cnt_blk. low physical
//  memory is identity-mapped in this kernel (the same assumption gpu_probe
//  relies on to dereference pci bars), so we can read these tables directly.

struct AcpiSDTHeader {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

static bool acpi_sig_eq(const char* a, const char* b4) {
    return a[0] == b4[0] && a[1] == b4[1] && a[2] == b4[2] && a[3] == b4[3];
}

// sum `len` bytes starting at `p`; valid acpi structures sum to 0 mod 256.
static uint8_t acpi_checksum(const void* p, uint32_t len) {
    const uint8_t* b = (const uint8_t*)p;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum = (uint8_t)(sum + b[i]);
    return sum;
}

// scan the legacy bios area for the rsdp ("RSD PTR " on a 16-byte boundary).
// returns the rsdp physical address, or 0 if not found. (satoru)
static uintptr_t acpi_find_rsdp() {
    const char sig[8] = {'R','S','D',' ','P','T','R',' '};
    for (uintptr_t addr = 0xE0000; addr < 0x100000; addr += 16) {
        const char* p = (const char*)addr;
        bool match = true;
        for (int i = 0; i < 8; i++) {
            if (p[i] != sig[i]) { match = false; break; }
        }
        if (!match) continue;
        // rsdp v1 checksum covers the first 20 bytes.
        if (acpi_checksum(p, 20) == 0) return addr;
    }
    return 0;
}

// locate the fadt ("FACP") by walking the rsdt (32-bit ptrs) or xsdt
// (64-bit ptrs). returns the fadt physical address, or 0. (satoru)
static uintptr_t acpi_find_fadt() {
    uintptr_t rsdp = acpi_find_rsdp();
    if (!rsdp) return 0;

    const uint8_t* r = (const uint8_t*)rsdp;
    uint8_t rev = r[15];

    // prefer the xsdt (64-bit) on acpi 2.0+ when its extended checksum is ok.
    if (rev >= 2) {
        uint32_t xsdt_len = *(const uint32_t*)(r + 20);
        if (xsdt_len >= 33 && acpi_checksum(r, xsdt_len) == 0) {
            uint64_t xsdt_addr = *(const uint64_t*)(r + 24);
            if (xsdt_addr) {
                const AcpiSDTHeader* xsdt = (const AcpiSDTHeader*)(uintptr_t)xsdt_addr;
                if (acpi_sig_eq(xsdt->signature, "XSDT") && xsdt->length >= sizeof(AcpiSDTHeader)) {
                    uint32_t entries = (xsdt->length - sizeof(AcpiSDTHeader)) / 8;
                    const uint8_t* arr = (const uint8_t*)xsdt + sizeof(AcpiSDTHeader);
                    for (uint32_t i = 0; i < entries; i++) {
                        uint64_t ent;
                        // entries are not guaranteed 8-byte aligned  -  copy.
                        const uint8_t* src = arr + i * 8;
                        uint8_t* dst = (uint8_t*)&ent;
                        for (int b = 0; b < 8; b++) dst[b] = src[b];
                        const AcpiSDTHeader* h = (const AcpiSDTHeader*)(uintptr_t)ent;
                        if (h && acpi_sig_eq(h->signature, "FACP")) return (uintptr_t)ent;
                    }
                }
            }
        }
    }

    // fall back to the rsdt (32-bit pointers).
    uint32_t rsdt_addr = *(const uint32_t*)(r + 16);
    if (!rsdt_addr) return 0;
    const AcpiSDTHeader* rsdt = (const AcpiSDTHeader*)(uintptr_t)rsdt_addr;
    if (!acpi_sig_eq(rsdt->signature, "RSDT") || rsdt->length < sizeof(AcpiSDTHeader)) return 0;
    uint32_t entries = (rsdt->length - sizeof(AcpiSDTHeader)) / 4;
    const uint8_t* arr = (const uint8_t*)rsdt + sizeof(AcpiSDTHeader);
    for (uint32_t i = 0; i < entries; i++) {
        uint32_t ent;
        const uint8_t* src = arr + i * 4;
        uint8_t* dst = (uint8_t*)&ent;
        for (int b = 0; b < 4; b++) dst[b] = src[b];
        const AcpiSDTHeader* h = (const AcpiSDTHeader*)(uintptr_t)ent;
        if (h && acpi_sig_eq(h->signature, "FACP")) return (uintptr_t)ent;
    }
    return 0;
}
}  // namespace

bool HAL::Suspend() {
    SerialLogger::Log("HAL: Suspend (ACPI S3) requested\r\n");

    // step 1 (implemented): recover pm1a_cnt_blk from the fadt. the fadt
    // (signature "FACP") stores pm1a_cnt_blk as a 32-bit io port at offset
    // 64 per the acpi spec. (satoru)
    uintptr_t fadt = acpi_find_fadt();
    if (!fadt) {
        SerialLogger::Log("HAL: Suspend aborted  -  no ACPI RSDP/FADT found\r\n");
        return false;
    }
    uint32_t pm1a_cnt = *(const uint32_t*)(fadt + 64);
    log_hex("HAL: FADT located, PM1a_CNT_BLK = ", pm1a_cnt);

    // step 2 (blocked): the s3 sleep type written to pm1a_cnt (slp_typa,
    // bits 10-12) is not stored in the fadt. it lives in the dsdt as the
    // aml object \_s3, e.g. Name(_S3, Package(){5,5,0,0}). recovering it
    // safely requires walking and decoding the dsdt's aml bytecode, which
    // needs a real aml interpreter this kernel does not have. writing a
    // guessed slp_typ (or skipping it) would either silently fail to sleep
    // or, worse, trigger an undefined hardware transition. (satoru)
    // TODO (satoru): requires ACPI FADT/DSDT parse for SLP_TYPx  -  decode the
    // \_S3 AML package in the DSDT to obtain SLP_TYPa/SLP_TYPb before we can
    // write (SLP_TYPa << 10) | SLP_EN to PM1a_CNT_BLK and enter S3.
    SerialLogger::Log("HAL: Suspend aborted  -  SLP_TYP for S3 requires DSDT AML parse (not implemented)\r\n");
    (void)pm1a_cnt;
    return false;
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
