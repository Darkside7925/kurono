#include "hal.h"
#include "../kernel/dbgwatch.h"   // kurono.dbgwatch (satoru)
#include "../drivers/serial.h"
#include "../proc/smp.h"          // per-cpu kernel-stack (gs:8) for the syscall path (satoru)
#include "../proc/spinlock.h"     // serialize cross-core exception dumps (satoru)
#include "../linux/linux_syscall.h"
#include "../kernel/panic.h"
#include "../proc/scheduler.h"
#include "../kernel/userspace.h"   // Userspace::HandleProcessExit for user-fault termination (satoru)
#include "../kernel/vmm.h"         // resolve user va->phys to dump the tls/tcb at a fault (satoru)
#include "../kernel/kdf.h"         // kdf guard-page fault isolation (catches driver oob -> no panic) (satoru)
#include "../kernel/kmemx.h"        // kmemx compressed-page fault decompression path (satoru)
#include "../drivers/timer.h"       // Timer::GetRealMs64 for the busy sampler (satoru)
#include "../kernel/kld_broker.h"   // kld msi vectors 0x50..0x57 -> broker doorbell (satoru)
#include "../kernel/taskmem.h"      // kurono.taskmem=1 interrupt marks (satoru)
#include "../drivers/disklog.h"      // flush the boot log before a reset (satoru)
#include "../kernel/pmm.h"          // per-ap kernel stacks on demand (satoru)
// busy MILLISECONDS per cpu (64-bit, no wrap), read by the task manager. charged
// by hal_sample_busy from each core's own timer irq using real elapsed time, so
// the bsp 1 kHz pit and the ap 100 Hz lapic timers need no shared rate. (satoru)
volatile uint64_t g_cpu_busy_ticks[HAL_CPU_SLOTS];
// the same per-cpu interval split by the ring the timer interrupted, plus the
// total sampled wall time: /proc/stat derives user/system/idle from these. (satoru)
volatile uint64_t g_cpu_total_ms[HAL_CPU_SLOTS];
volatile uint64_t g_cpu_user_ms[HAL_CPU_SLOTS];
volatile uint64_t g_cpu_sys_ms[HAL_CPU_SLOTS];
// busy time no task owns (interrupt work, early boot): the kernel's own share.
volatile uint64_t g_cpu_kernel_ms[HAL_CPU_SLOTS];
// per-pid busy milliseconds side table (struct Process stays untouched). 1024
// slots, open addressing, keyed by pid and validated against the process's
// kernel stack top so a recycled pid never inherits old counters. the old
// 256-slot pid & 255 table re-tagged on every collision, so with firefox's ~45
// threads plus the kernel tasks it lost most of the per-process cpu time. (satoru)
struct HalPidBusy {
    volatile uint32_t pid;
    volatile uint32_t stamp;     // low 32 bits of kernel_stack_top (satoru)
    volatile uint8_t  dead;      // tombstone: keep probing past it, reusable (satoru)
    volatile uint64_t ms;
    volatile uint64_t user_ms;
    volatile uint64_t sys_ms;
};
static const uint32_t HAL_PID_SLOTS = 1024;
static HalPidBusy g_pid_busy[HAL_PID_SLOTS];
static uint64_t   g_busy_last_ms[HAL_CPU_SLOTS];
static inline uint32_t hal_slot(uint32_t pid) {
    return (pid * 2654435761u) & (HAL_PID_SLOTS - 1u);
}
// open addressing with tombstones: a forget must not punch a hole that hides
// entries inserted after it. dead 2 = a writer is claiming the slot. (satoru)
static HalPidBusy* hal_pid_find(uint32_t pid, uint32_t stamp, bool create) {
    uint32_t i = hal_slot(pid);
    for (uint32_t n = 0; n < HAL_PID_SLOTS; n++) {
        HalPidBusy& s = g_pid_busy[i];
        uint32_t cur = __atomic_load_n(&s.pid, __ATOMIC_ACQUIRE);
        uint8_t dead = __atomic_load_n(&s.dead, __ATOMIC_ACQUIRE);
        if (cur == 0) {
            if (!create) return nullptr;
            uint32_t zero = 0;
            if (!__atomic_compare_exchange_n(&s.pid, &zero, pid, false,
                                             __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
                continue;
            __atomic_store_n(&s.dead, (uint8_t)1, __ATOMIC_RELEASE);
            __atomic_store_n(&s.ms, 0ull, __ATOMIC_RELEASE);
            __atomic_store_n(&s.user_ms, 0ull, __ATOMIC_RELEASE);
            __atomic_store_n(&s.sys_ms, 0ull, __ATOMIC_RELEASE);
            __atomic_store_n(&s.stamp, stamp, __ATOMIC_RELEASE);
            __atomic_store_n(&s.dead, (uint8_t)0, __ATOMIC_RELEASE);
            return &s;
        }
        if (dead) {
            if (!create) { i = (i + 1u) & (HAL_PID_SLOTS - 1u); continue; }
            uint8_t one = 1;
            if (!__atomic_compare_exchange_n(&s.dead, &one, (uint8_t)2, false,
                                             __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
                i = (i + 1u) & (HAL_PID_SLOTS - 1u);
                continue;
            }
            __atomic_store_n(&s.pid, pid, __ATOMIC_RELEASE);
            __atomic_store_n(&s.stamp, stamp, __ATOMIC_RELEASE);
            __atomic_store_n(&s.ms, 0ull, __ATOMIC_RELEASE);
            __atomic_store_n(&s.user_ms, 0ull, __ATOMIC_RELEASE);
            __atomic_store_n(&s.sys_ms, 0ull, __ATOMIC_RELEASE);
            __atomic_store_n(&s.dead, (uint8_t)0, __ATOMIC_RELEASE);
            return &s;
        }
        if (cur == pid) {
            uint32_t have = __atomic_load_n(&s.stamp, __ATOMIC_ACQUIRE);
            if (have == stamp) return &s;
            if (!create) return nullptr;
            __atomic_store_n(&s.ms, 0ull, __ATOMIC_RELEASE);
            __atomic_store_n(&s.user_ms, 0ull, __ATOMIC_RELEASE);
            __atomic_store_n(&s.sys_ms, 0ull, __ATOMIC_RELEASE);
            __atomic_store_n(&s.stamp, stamp, __ATOMIC_RELEASE);
            return &s;
        }
        i = (i + 1u) & (HAL_PID_SLOTS - 1u);
    }
    return nullptr;   // full: drop the sample rather than corrupt another pid's row (satoru)
}
static inline uint32_t hal_stamp(const Process* p) {
    return p ? (uint32_t)(p->kernel_stack_top >> 12) : 0u;
}
uint64_t hal_pid_busy_ms(uint32_t pid) {
    uint32_t i = hal_slot(pid);
    for (uint32_t n = 0; n < HAL_PID_SLOTS; n++) {
        HalPidBusy& e = g_pid_busy[i];
        uint32_t cur = __atomic_load_n(&e.pid, __ATOMIC_ACQUIRE);
        if (cur == 0) break;
        if (cur == pid && !__atomic_load_n(&e.dead, __ATOMIC_ACQUIRE))
            return __atomic_load_n(&e.ms, __ATOMIC_RELAXED);
        i = (i + 1u) & (HAL_PID_SLOTS - 1u);
    }
    return 0ull;
}
void hal_pid_cpu_ms(uint32_t pid, uint64_t* user_ms, uint64_t* sys_ms, uint64_t* total_ms) {
    uint64_t u = 0, sy = 0, t = 0;
    uint32_t i = hal_slot(pid);
    for (uint32_t n = 0; n < HAL_PID_SLOTS; n++) {
        HalPidBusy& e = g_pid_busy[i];
        uint32_t cur = __atomic_load_n(&e.pid, __ATOMIC_ACQUIRE);
        if (cur == 0) break;
        if (cur == pid && !__atomic_load_n(&e.dead, __ATOMIC_ACQUIRE)) {
            u = __atomic_load_n(&e.user_ms, __ATOMIC_RELAXED);
            sy = __atomic_load_n(&e.sys_ms, __ATOMIC_RELAXED);
            t = __atomic_load_n(&e.ms, __ATOMIC_RELAXED);
            break;
        }
        i = (i + 1u) & (HAL_PID_SLOTS - 1u);
    }
    if (user_ms)  *user_ms  = u;
    if (sys_ms)   *sys_ms   = sy;
    if (total_ms) *total_ms = t;
}
// drop a pid's counters when its task is reaped, so a recycled pid starts at
// zero even if the destroy path races a sampler. (satoru)
void hal_pid_cpu_forget(uint32_t pid) {
    uint32_t i = hal_slot(pid);
    for (uint32_t n = 0; n < HAL_PID_SLOTS; n++) {
        HalPidBusy& e = g_pid_busy[i];
        uint32_t cur = __atomic_load_n(&e.pid, __ATOMIC_ACQUIRE);
        if (cur == pid) {
            __atomic_store_n(&e.ms, 0ull, __ATOMIC_RELEASE);
            __atomic_store_n(&e.user_ms, 0ull, __ATOMIC_RELEASE);
            __atomic_store_n(&e.sys_ms, 0ull, __ATOMIC_RELEASE);
            __atomic_store_n(&e.stamp, 0u, __ATOMIC_RELEASE);
            __atomic_store_n(&e.dead, (uint8_t)1, __ATOMIC_RELEASE);
            return;
        }
        if (cur == 0) return;
        i = (i + 1u) & (HAL_PID_SLOTS - 1u);
    }
}
// kernel time no task row owns, in milliseconds over all cpus. (satoru)
uint64_t hal_kernel_cpu_ms() {
    uint64_t sum = 0;
    for (int c = 0; c < HAL_CPU_SLOTS; c++) sum += g_cpu_kernel_ms[c];
    return sum;
}
// timer-irq sampler: this core was busy for the real time since its last tick if
// it was in ring 3, or in ring 0 running a non-idle task that is not parked on
// hlt. idle (pid 0 / hlt) is never counted. (satoru)
static void hal_sample_busy(InterruptFrame* frame, uint32_t cpu) {
    if (cpu >= HAL_CPU_SLOTS) return;   // was cpu & 15: cpus 16-19 counted as 0-3 (satoru)
    uint64_t now = Timer::GetRealMs64();
    uint64_t last = g_busy_last_ms[cpu];
    if (now == last) return;
    g_busy_last_ms[cpu] = now;
    if (last == 0) return;
    uint64_t dt = now - last;
    if (dt > 200) dt = 200;
    g_cpu_total_ms[cpu] += dt;
    Process* cur = Scheduler::GetCurrentProcess();
    bool user = ((frame->cs & 3) == 3);
    // a user frame can never be hlt (cpl3 hlt faults), and its code page at
    // rip-1 may be an unmapped guard page (llvmpipe jit regions): reading it
    // from the timer irq panicked the kernel. (satoru)
    bool hlt = !user && frame->rip > 0x1000 && *(volatile uint8_t*)(frame->rip - 1) == 0xF4;
    // kernel work with no task of its own (early boot, scheduler idle paths) is
    // the kernel's own usage; idle hlt is never counted. (satoru)
    bool kernel_own = !user && !hlt && (!cur || cur->pid == 0);
    bool busy = user || (cur && cur->pid != 0 && cur->state == Process_Running && !hlt);
    if (!busy && !kernel_own) return;
    g_cpu_busy_ticks[cpu] += dt;
    if (user) g_cpu_user_ms[cpu] += dt; else g_cpu_sys_ms[cpu] += dt;
    if (kernel_own) { g_cpu_kernel_ms[cpu] += dt; return; }
    if (cur && cur->pid != 0) {
        HalPidBusy* s = hal_pid_find(cur->pid, hal_stamp(cur), true);
        if (!s) return;   // table full: skip, never charge another pid (satoru)
        __atomic_add_fetch(&s->ms, dt, __ATOMIC_RELAXED);
        if (user) __atomic_add_fetch(&s->user_ms, dt, __ATOMIC_RELAXED);
        else      __atomic_add_fetch(&s->sys_ms, dt, __ATOMIC_RELAXED);
    }
}
// test rig (kurono.ffcrash=S): crash firefox once, S s after exec, by pointing a
// ring-3 bsp thread of its address space at rip 0 on a timer tick. (satoru)
volatile uint64_t g_ffcrash_as = 0;
volatile uint64_t g_ffcrash_at = 0;
volatile uint32_t g_ffcrash_secs = 0;
volatile int      g_ffreopen_left = 0;

//  x86_64 idt, pic 8259a, and isr implementation
//  isr stubs live in isr_stubs.asm - this file builds the idt from the
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
    extern void isr_stub_65();            // tlb-shootdown ipi (smp thread dispatch) (satoru)
    extern void isr_stub_66();            // sched=2 resched ipi (satoru)
    // kld broker msi vectors 0x50..0x57 (satoru)
    extern void isr_stub_80(); extern void isr_stub_81(); extern void isr_stub_82(); extern void isr_stub_83();
    extern void isr_stub_84(); extern void isr_stub_85(); extern void isr_stub_86(); extern void isr_stub_87();
    extern char isr_stray_base[];         // 256 stray-vector stubs, 16 bytes apart (satoru)
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
    // dedicated double-fault stack: a #DF caused by a bad/overflowed kernel rsp
    // pushes its frame onto that same bad rsp when ist=0, so the handler never
    // runs and the machine silently triple-faults (the unreported lottery
    // boots). ist2 gives vector 8 a known-good stack so the raw fault report
    // always makes it out. (satoru)
    alignas(16) static uint8_t bsp_df_stack[8192];
    system_tss.ist2 = (uint64_t)(uintptr_t)(bsp_df_stack + sizeof(bsp_df_stack));
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
// per-ap kernel entry + double-fault stacks. only the cpus this machine
// actually brought up need them, so each is taken from the pmm when that ap
// starts instead of reserving 24 kb for all 32 possible cpus. (satoru)
static uint8_t* ap_priv_stack[SMP_MAX_CPUS];
static uint8_t* ap_df_stack[SMP_MAX_CPUS];

void HAL::SetupAPCpuState() {
    uint32_t cpu = SMP::CpuIndex();
    if (cpu == 0 || cpu >= SMP_MAX_CPUS) return;   // bsp already set up via InitGDT (satoru)

    for (int i = 0; i < 5; i++) ap_gdt[cpu][i].value = gdt_entries[i].value;

    if (!ap_priv_stack[cpu]) {
        ap_priv_stack[cpu] = (uint8_t*)PMM::AllocBytes(16384);
        if (!ap_priv_stack[cpu]) KernelPanic::KeBugCheckEx(StopCode::OUT_OF_MEMORY,
            cpu, 0, 0, 0, "ap kernel stack alloc failed", __FILE__, (uint32_t)__LINE__);
    }
    if (!ap_df_stack[cpu]) {
        ap_df_stack[cpu] = (uint8_t*)PMM::AllocBytes(8192);
        if (!ap_df_stack[cpu]) KernelPanic::KeBugCheckEx(StopCode::OUT_OF_MEMORY,
            cpu, 0, 0, 0, "ap double-fault stack alloc failed", __FILE__, (uint32_t)__LINE__);
    }
    TSS64* t = &ap_tss[cpu];
    for (size_t i = 0; i < sizeof(TSS64); i++) ((uint8_t*)t)[i] = 0;
    t->rsp0 = (uint64_t)(uintptr_t)(ap_priv_stack[cpu] + 16384);
    t->ist1 = t->rsp0;
    t->ist2 = (uint64_t)(uintptr_t)(ap_df_stack[cpu] + 8192);
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

    // icw2: vector base offsets - irq0-7 → 32-39, irq8-15 → 40-47
    OutByte(PIC1_DATA, 0x20);  IOWait();
    OutByte(PIC2_DATA, 0x28);  IOWait();

    // icw3: master has slave on irq2 (bit 2), slave cascade identity = 2
    OutByte(PIC1_DATA, 0x04);  IOWait();
    OutByte(PIC2_DATA, 0x02);  IOWait();

    // icw4: 8086/88 mode, normal eoi
    OutByte(PIC1_DATA, 0x01);  IOWait();
    OutByte(PIC2_DATA, 0x01);  IOWait();

    // ocw1: mask all irqs initially - the kernel will selectively enable them
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

//  common c interrupt handler - called from isr_stubs.asm
static void log_hex(const char* prefix, uint64_t val) {
    // room for a long descriptive prefix: the old 32-byte buffer let a prefix
    // past ~11 chars overwrite the return address (the acpi power-button line
    // crashed the worker on the first real boot). (satoru)
    char buf[128];
    const char* hex = "0123456789ABCDEF";
    int len = 0;
    while (prefix[len] && len < 100) { buf[len] = prefix[len]; len++; }
    buf[len++] = '0'; buf[len++] = 'x';
    for (int i = 60; i >= 0; i -= 4) {
        buf[len++] = hex[(val >> i) & 0xF];
    }
    buf[len++] = '\r'; buf[len++] = '\n'; buf[len] = 0;
    SerialLogger::Log(buf);
}

// sched=2 ring-3 fault containment on an application processor: kill the
// faulting address-space group and iret this cpu back into its dispatch loop,
// instead of parking the core for the rest of the session. the frame lives on
// the dead task's kernel stack; the cpu leaves that stack at the iretq, and the
// deferred release (ReleaseOnCpu -> FinishSwitch in the dispatch loop) only then
// lets a reaper free it. returns true if it rewrote the frame. (satoru)
static bool sched2_contain_ap_fault(InterruptFrame* frame) {
    if (!Scheduler::SchedV2() || SMP::CpuIndex() == 0) return false;
    Process* dead = Scheduler::GetCurrentProcess();
    // leave the dead group's cr3 BEFORE this cpu stops looking busy: once
    // current is null a launcher may tear the address space down, and this cpu
    // still has to run the rest of this handler. (satoru)
    SMP::LoadKernelCr3();
    if (dead && dead->is_user()) {
        Process* grp[256];   // firefox runs ~50+ threads; 64 left a tail alive (satoru)
        int gn = Scheduler::CollectAddressSpaceGroup(dead->pid, grp, 256);
        for (int i = 0; i < gn; i++) Scheduler::MarkProcessExited(grp[i], 139);
        if (gn == 0) { Scheduler::MarkProcessExited(dead, 139); grp[0] = dead; gn = 1; }
        // the fault kill used to mark the group but never queue it for a reap,
        // so its address space (and every frame of a static low image) stayed
        // owned until reboot. queue it now: the drain keeps the members on
        // reaped=2 until they are all off every cpu, then tears the space down.
        // (satoru)
        Scheduler::QueueDeferredReap(grp, gn);
        Scheduler::ReleaseOnCpu(dead);
    }
    Scheduler::SetCurrentForThisCpu(nullptr);
    SerialLogger::Log("[sched2] ring-3 fault contained: group killed, cpu back to dispatch\r\n");
    SMP::ApIdleFrame(frame);
    return true;
}

// kurono.taskmem=1: heap allocations made while this mark is up are interrupt
// work and go to "kernel", not to the interrupted task. restores the outer
// mark on every return path. (satoru)
struct TmIrqMark {
    bool on; uint32_t cpu; uint64_t prev;
    explicit TmIrqMark(const void* f) : on(TaskMem::g_on), cpu(0), prev(on ? TaskMem::IrqEnter(f, &cpu) : 0) {}
    ~TmIrqMark() { if (on) TaskMem::IrqLeave(cpu, prev); }
};

// ── vector 0 that is not a divide (the laptop's kurono.sched=2 panic) ────────
// the 1110 laptop boot died with "#DE on cpu0" at the instruction right after the
// popf in VMM::RunVCPU, with the alpine vcpu loop on cpu0. that popf is where
// every host interrupt is taken while a guest runs (an external interrupt only
// causes the exit; the host takes it when IF comes back), so the rip says the
// event was an INTERRUPT on vector 0, not a division. a real #DE is a fault:
// its rip names the div / idiv itself. on real hardware the lapic never
// delivers vectors 0-15 (sdm "illegal vector"), so the candidates are an 8259
// whose vector base was reprogrammed (extint through lint0 carries the pic's
// own vector) or a corrupted idt entry. this reports both, repairs the pic and
// carries on; the same rip again and again is a real fault after all. (satoru)
static bool insn_is_divide(uint64_t rip) {
    if (rip < 0x100000ULL) return true;   // cannot read it safely: treat as a real fault (satoru)
    const volatile uint8_t* b = (const volatile uint8_t*)(uintptr_t)rip;
    int i = 0;
    for (; i < 6; i++) {
        uint8_t x = b[i];
        bool pfx = (x >= 0x40 && x <= 0x4F) || x == 0x66 || x == 0x67 || x == 0xF0 ||
                   x == 0xF2 || x == 0xF3 || x == 0x2E || x == 0x3E || x == 0x26 ||
                   x == 0x36 || x == 0x64 || x == 0x65;
        if (!pfx) break;
    }
    uint8_t op = b[i];
    if (op != 0xF6 && op != 0xF7) return false;
    uint8_t reg = (uint8_t)((b[i + 1] >> 3) & 7);
    return reg == 6 || reg == 7;   // div, idiv (satoru)
}

static uint64_t g_vec0_hits = 0, g_vec0_last_rip = 0, g_vec0_same = 0, g_vec0_last_ms = 0;
static uint64_t g_vec0_win_ms = 0, g_vec0_win_n = 0;   // storm cap (satoru)

// the same event on another exception vector (the 1112 laptop boot: #OF at the
// instruction after `sti; hlt` with cpu0 idle). a vector that pushes no error code,
// taken in ring 0 right after a hlt, cannot be its exception: hlt raises none and
// the next instruction has not run. #OF and #BR cannot happen at all in long mode
// (into / bound are #UD there). nmi, #MC and the error-code vectors are never
// claimed (an interrupt there would leave a shifted frame). (satoru)
static bool stray_exception_vector(uint64_t vec, const InterruptFrame* frame) {
    if ((frame->cs & 3) != 0) return false;
    if (vec == 0) return !insn_is_divide(frame->rip);
    if (vec == 2 || vec == 8 || vec == 18) return false;
    if ((vec >= 10 && vec <= 14) || vec == 17 || vec == 21 || vec == 29 || vec == 30) return false;
    if (vec >= 32) return false;
    if (vec == 4 || vec == 5) return true;
    if (frame->rip < 0x100001ULL) return false;
    if ((frame->rip & 0xFFF) == 0) return false;   // rip-1 is on another page, maybe unmapped (satoru)
    return *(const volatile uint8_t*)(uintptr_t)(frame->rip - 1) == 0xF4;   // hlt (satoru)
}

static bool vec0_interrupt_recover(InterruptFrame* frame) {
    if (!stray_exception_vector(frame->vector, frame)) return false;
    uint64_t now = Timer::GetRealMs64();
    if (frame->rip == g_vec0_last_rip && now - g_vec0_last_ms < 10) {
        if (++g_vec0_same >= 8) return false;   // a fault loop at one rip: let it panic (satoru)
    } else {
        g_vec0_same = 0;
    }
    g_vec0_last_rip = frame->rip; g_vec0_last_ms = now;
    // a stuck source re-firing after every iretq lands at a new rip each time:
    // more than 500 in one second is a storm, not a stray event - panic. (satoru)
    if (now - g_vec0_win_ms >= 1000) { g_vec0_win_ms = now; g_vec0_win_n = 0; }
    if (++g_vec0_win_n > 500) return false;
    uint64_t n = ++g_vec0_hits;
    // the 8259 state, read before anything acks it. ocw3 0x0a = irr, 0x0b = isr. (satoru)
    HAL::OutByte(0x20, 0x0A); uint8_t m_irr = HAL::InByte(0x20);
    HAL::OutByte(0x20, 0x0B); uint8_t m_isr = HAL::InByte(0x20);
    HAL::OutByte(0xA0, 0x0A); uint8_t s_irr = HAL::InByte(0xA0);
    HAL::OutByte(0xA0, 0x0B); uint8_t s_isr = HAL::InByte(0xA0);
    uint8_t m_mask = HAL::InByte(0x21), s_mask = HAL::InByte(0xA1);
    bool pic_irq = (m_isr | s_isr) != 0;
    if (n <= 8 || (n & 1023) == 0) {
        SerialLogger::Log("[vec0] vector "); SerialLogger::LogDec((int)frame->vector);
        SerialLogger::Log(" delivered at a non-faulting rip=0x"); SerialLogger::LogHex64(frame->rip);
        SerialLogger::Log(" cpu="); SerialLogger::LogDec((int)SMP::CpuIndex());
        SerialLogger::Log(" n="); SerialLogger::LogDec((int)n);
        SerialLogger::Log(" pic isr="); SerialLogger::LogHex(m_isr); SerialLogger::Log("/"); SerialLogger::LogHex(s_isr);
        SerialLogger::Log(" irr="); SerialLogger::LogHex(m_irr); SerialLogger::Log("/"); SerialLogger::LogHex(s_irr);
        SerialLogger::Log(" mask="); SerialLogger::LogHex(m_mask); SerialLogger::Log("/"); SerialLogger::LogHex(s_mask);
        // lapic in-service / requested vectors and the lvt entries: a vector-0
        // lvt or an ipi with vector 0 would show up here. (satoru)
        SerialLogger::Log(" lapic isr:");
        for (uint32_t r = 0; r < 8; r++) {
            uint32_t v = SMP::LapicRead(0x100 + r * 0x10);
            for (uint32_t b = 0; b < 32; b++) if (v & (1u << b)) { SerialLogger::Log(" "); SerialLogger::LogHex(r * 32 + b); }
        }
        SerialLogger::Log(" irr:");
        for (uint32_t r = 0; r < 8; r++) {
            uint32_t v = SMP::LapicRead(0x200 + r * 0x10);
            for (uint32_t b = 0; b < 32; b++) if (v & (1u << b)) { SerialLogger::Log(" "); SerialLogger::LogHex(r * 32 + b); }
        }
        SMP::LapicWrite(0x280, 0);
        SerialLogger::Log(" esr="); SerialLogger::LogHex(SMP::LapicRead(0x280));
        SerialLogger::Log(" lvt t/th/pc/l0/l1/err=");
        static const uint32_t lvt[6] = { 0x320, 0x330, 0x340, 0x350, 0x360, 0x370 };
        for (int k = 0; k < 6; k++) { SerialLogger::LogHex(SMP::LapicRead(lvt[k])); SerialLogger::Log(k < 5 ? "/" : ""); }
        uint64_t h0 = (uint64_t)idt_entries[0].offset_low | ((uint64_t)idt_entries[0].offset_mid << 16) |
                      ((uint64_t)idt_entries[0].offset_high << 32);
        uint64_t h32 = (uint64_t)idt_entries[32].offset_low | ((uint64_t)idt_entries[32].offset_mid << 16) |
                       ((uint64_t)idt_entries[32].offset_high << 32);
        SerialLogger::Log(" idt0="); SerialLogger::LogHex64(h0);
        SerialLogger::Log(h0 == isr_stub_table[0] ? "(ok)" : "(CHANGED)");
        SerialLogger::Log(" idt32="); SerialLogger::LogHex64(h32);
        SerialLogger::Log(h32 == isr_stub_table[32] ? "(ok)" : "(CHANGED)");
        SerialLogger::Log("\r\n");
    }
    // repair what can be repaired: an idt entry that no longer names its stub,
    // and an 8259 that delivered an irq on vector 0 (its base was rewritten):
    // re-initialize it with the masks it had and end the interrupt. (satoru)
    for (int v = 0; v < 48; v++) {
        uint64_t h = (uint64_t)idt_entries[v].offset_low | ((uint64_t)idt_entries[v].offset_mid << 16) |
                     ((uint64_t)idt_entries[v].offset_high << 32);
        if (h != isr_stub_table[v]) idt_set(v, isr_stub_table[v], v == 8 ? 2 : 0, 0);
    }
    if (pic_irq) {
        HAL::OutByte(0x20, 0x11); HAL::OutByte(0xA0, 0x11);
        HAL::OutByte(0x21, 0x20); HAL::OutByte(0xA1, 0x28);
        HAL::OutByte(0x21, 0x04); HAL::OutByte(0xA1, 0x02);
        HAL::OutByte(0x21, 0x01); HAL::OutByte(0xA1, 0x01);
        HAL::OutByte(0x21, m_mask); HAL::OutByte(0xA1, s_mask);
        if (n <= 8) SerialLogger::Log("[vec0] 8259 re-initialized (base 0x20/0x28, masks kept)\r\n");
    }
    return true;
}

// an interrupt on a vector nothing set up (see InitIDT). end it where it is in
// service: the lapic when its isr bit is set (a missing eoi would block every
// vector of that priority class on this cpu), the 8259 when it has an irq in
// service (its inta came back with a wrong vector, so the irq handler never ran
// and never sent the eoi: the pit tick would stop for good). the spurious vector
// 0xff is never in service and gets no eoi. (satoru)
static uint64_t g_stray_hits = 0;
static void hal_stray_vector(InterruptFrame* frame) {
    uint32_t vec = (uint32_t)frame->vector;
    bool lapic_isr = false;
    if (vec >= 16 && vec < 256)
        lapic_isr = (SMP::LapicRead(0x100 + (vec >> 5) * 0x10) >> (vec & 31)) & 1;
    if (lapic_isr) SMP::LapicWrite(0xB0, 0);
    HAL::OutByte(0x20, 0x0B); uint8_t m_isr = HAL::InByte(0x20);
    HAL::OutByte(0xA0, 0x0B); uint8_t s_isr = HAL::InByte(0xA0);
    if (s_isr) HAL::OutByte(0xA0, 0x20);
    if (m_isr) HAL::OutByte(0x20, 0x20);
    uint64_t n = __atomic_add_fetch(&g_stray_hits, 1, __ATOMIC_RELAXED);
    if (n <= 16 || (n & 1023) == 0) {
        SerialLogger::Log("[stray] vector 0x"); SerialLogger::LogHex(vec);
        SerialLogger::Log(" cpu="); SerialLogger::LogDec((int)SMP::CpuIndex());
        SerialLogger::Log(" rip=0x"); SerialLogger::LogHex64(frame->rip);
        SerialLogger::Log(" n="); SerialLogger::LogDec((int)n);
        SerialLogger::Log(lapic_isr ? " lapic-eoi" : "");
        SerialLogger::Log(" pic isr="); SerialLogger::LogHex(m_isr); SerialLogger::Log("/"); SerialLogger::LogHex(s_isr);
        SerialLogger::Log("\r\n");
    }
}

void HAL::StrayVectorSelfTest() {
    auto ticks = []() { return HAL::pit_ticks; };
    SerialLogger::Log("[straytest] start cpu="); SerialLogger::LogDec((int)SMP::CpuIndex());
    SerialLogger::Log(SMP::BspLapicTickActive() ? " tick=lapic\r\n" : " tick=pit\r\n");
    uint64_t t0 = ticks();
    Scheduler::SleepMs(20);
    uint64_t t1 = ticks();
    __asm__ volatile("int $0xA9");                  // software stray: stub, no eoi (satoru)
    SMP::TestSelfIpi(0xA9);                         // hardware stray: lapic isr bit, needs the eoi (satoru)
    Scheduler::SleepMs(20);
    uint64_t t2 = ticks();
    __asm__ volatile("int $4");                     // #OF vector, impossible as a fault in long mode (satoru)
    Scheduler::SleepMs(20);
    uint64_t t3 = ticks();
    // the laptop's case: the 8259 hands out vector 0 for irq0. (satoru)
    __asm__ volatile("cli");
    uint8_t m = HAL::InByte(0x21), sm = HAL::InByte(0xA1);
    HAL::OutByte(0x20, 0x11); HAL::OutByte(0x21, 0x00); HAL::OutByte(0x21, 0x04); HAL::OutByte(0x21, 0x01);
    HAL::OutByte(0x21, (uint8_t)(m & ~1u));
    __asm__ volatile("sti");
    Scheduler::SleepMs(20);
    __asm__ volatile("cli");
    if (SMP::BspLapicTickActive()) HAL::DisableIRQ(0);
    else HAL::OutByte(0x21, m);
    HAL::OutByte(0xA1, sm);
    __asm__ volatile("sti");
    Scheduler::SleepMs(20);
    uint64_t t4 = ticks();
    SerialLogger::Log("[straytest] ticks per 20 ms: base="); SerialLogger::LogDec((int)(t1 - t0));
    SerialLogger::Log(" after-0xa9="); SerialLogger::LogDec((int)(t2 - t1));
    SerialLogger::Log(" after-int4="); SerialLogger::LogDec((int)(t3 - t2));
    SerialLogger::Log(" after-pic0(40ms)="); SerialLogger::LogDec((int)(t4 - t3));
    SerialLogger::Log(" strays="); SerialLogger::LogDec((int)g_stray_hits);
    SerialLogger::Log(" vec0="); SerialLogger::LogDec((int)g_vec0_hits);
    SerialLogger::Log((t2 > t1 && t3 > t2 && t4 > t3 && g_stray_hits >= 2 && g_vec0_hits >= 1) ? " PASS\r\n" : " FAIL\r\n");
}

void UbTraceIsrIn(InterruptFrame* f);   // [ubchk], end of file (satoru)
extern "C" void isr_common_handler(InterruptFrame* frame) {
    uint64_t vec = frame->vector;
    UbTraceIsrIn(frame);   // [ubchk] (satoru)

    // the panicking cpu's halt request (try_halt_other_cpus): park at once. the
    // full exception dump this nmi used to get read past its stack top into a
    // guard page and re-entered the panic from this cpu (1112 laptop). (satoru)
    // a real nmi on the panicking cpu itself is ignored: parking it would stop
    // the countdown and the reset. (satoru)
    if (vec == 2 && KernelPanic::Panicking()) {
        if (SMP::CpuIndex() == KernelPanic::PanicCpu()) return;
        for (;;) asm volatile("cli; hlt");
    }

    // sched=2 stack hand-off: publish on_cpu=0 for a task this cpu switched away
    // from, once rsp is off its kernel stack (FinishSwitch checks rsp itself, so
    // a nested interrupt still on that stack is a no-op). interrupts and ring-3
    // faults only; a ring-0 fault path is left untouched. (satoru)
    if (vec >= 32 || (frame->cs & 3) == 3) Scheduler::FinishSwitch();

    // kurono.wp: re-arm CR0.WP if a read-only kernel store suspended it on this
    // cpu (SMP::WpKernelWriteFallback), once past that store. one load when
    // nothing is suspended. (satoru)
    SMP::WpRearm(frame->rip);

    // kurono.dbgwatch: a hardware write watchpoint hit (#DB trap) (satoru)
    if (vec == 1 && DbgWatch::OnDebugTrap(frame)) return;

    if (vec == 0x80) {
        if (syscall_handler) {
            syscall_handler(frame);
        } else {
            frame->rax = (uint64_t)-1;
        }
        return;
    }

    if (vec == 32 && g_ffcrash_as && (frame->cs & 3) == 3 && Scheduler::NowMs() >= g_ffcrash_at) {
        Process* fc = Scheduler::GetCurrentProcess();
        if (fc && fc->address_space == g_ffcrash_as) {
            g_ffcrash_as = 0;
            SerialLogger::Log("[ffcrash] injected rip=0 into pid ");
            SerialLogger::LogDec((int)fc->pid);
            SerialLogger::Log("\r\n");
            frame->rip = 0;
        }
    }

    // per-AP LAPIC timer (smp phase 4): preempt the user thread this ap is running,
    // then signal end-of-interrupt to the local apic. fires only while ring-3 user
    // code runs (ApTimerPreempt no-ops otherwise). (satoru)
    if (vec == 0x40) {
        // per-core load side table for the task manager: busy ms from real
        // elapsed time on this cpu. no effect on scheduling. (satoru)
        hal_sample_busy(frame, SMP::Current()->cpu_index);
        Scheduler::ServiceCpuTick();   // kurono.svccpu: a service cpu's kernel procs (satoru)
        Scheduler::ApTimerPreempt(frame);
        SMP::LapicWrite(0xB0, 0);   // lapic EOI register (satoru)
        return;
    }

    // tlb-shootdown ipi: reload cr3 (flushes non-global tlb entries) and ack so
    // the sender can proceed knowing this core holds no stale user translation
    // for the just-unmapped/reprotected range. (satoru)
    if (vec == 0x41) {
        SMP::HandleTlbIpi();
        SMP::LapicWrite(0xB0, 0);   // lapic EOI register (satoru)
        return;
    }

    // sched=2 resched ipi: the wake already happened in memory; the interrupt
    // only exists to break the target out of hlt. eoi and return. (satoru)
    if (vec == 0x42) {
        SMP::LapicWrite(0xB0, 0);
        // from ring 3: a kick for a pending signal or a kill - act on it at
        // this return to user mode. (satoru)
        if ((frame->cs & 3) == 3) Scheduler::ApUserIrqCheck(frame);
        return;
    }

    // kld broker msi (vectors 0x50..0x57): doorbell + wake only, then lapic eoi.
    // msi goes straight to the lapic, never through the 8259. (satoru)
    if (vec >= 0x50 && vec < 0x58) {
        TmIrqMark _tm(frame);   // (satoru)
        KldBroker::OnVector((uint32_t)vec);
        SMP::LapicWrite(0xB0, 0);   // lapic EOI register (satoru)
        return;
    }

    if (vec < 32) {
        // guarded kernel stacks: an overflow where the #pf frame push itself hit
        // the guard comes in as #DF (ist2) with cr2 still naming the guard. (satoru)
        if (vec == 8 && Scheduler::KstackGuardHit(frame->cr2)) return;
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
            // 1.7) KMemX compressed-page fault: if CR2 is a page kmemx compressed
            //      out (its leaf is not-present + carries PTE_KMEMX_COMPRESSED),
            //      decompress it from the pool back into a fresh frame, restore the
            //      leaf, and resume - the faulting instruction is then retried over
            //      the now-present page. crc32 is verified inside; a mismatch
            //      panics (never hand back wrong bytes). this is the transparent
            //      "touch a cold page, get it back in a few microseconds" path. it
            //      claims only kmemx-marked leaves, so every other fault falls
            //      through to the normal user/kernel handling below. runs before
            //      the demand-zero dispatch so a compressed page is never mistaken
            //      for a fresh anonymous page. (satoru)
            if (KMemX::HandleFault(frame->cr2)) {
                return;
            }
            // 2) User-mode page-fault dispatch (demand-zero, COW, etc.).
            //
            //    A ring-3 fault can be a RESTARTABLE faulting SSE store: e.g.
            //    musl's memcpy fast path `movups %xmm0,(mem)` faulting on the
            //    first byte of a fresh demand-zero page. After we map the page
            //    and IRET, the cpu RE-EXECUTES that store from %xmm0 - so %xmm0
            //    (the whole fpu/sse state) must be byte-identical to what the
            //    faulting code held. But the #pf path runs kernel code that
            //    touches xmm (memcpy/graphics inline asm, an IRQ-driven task
            //    switch's fxrstor), clobbering %xmm0. That silently zeroed the
            //    first 16 bytes of the page the restarted movups wrote - exactly
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
                // ring-0 write fault on a present user page nothing resolved:
                // with CR0.WP on, run it the WP=0 way and log it instead of
                // panicking (no uaccess fixup to return -EFAULT through). (satoru)
                if ((frame->error_code & 0x3) == 0x3 && frame->cr2 <= USER_SPACE_TOP) {
                    Process* wt = Scheduler::GetCurrentProcess();
                    uint64_t wpf = (wt && wt->is_user())
                        ? KernelVMM::QueryPageFlagsInAddressSpace(wt->address_space, frame->cr2 & ~0xFFFULL)
                        : KernelVMM::QueryPageFlagsInAddressSpace(KernelVMM::GetCurrentAddressSpace(), frame->cr2 & ~0xFFFULL);
                    if (SMP::WpKernelWriteFallback(frame->cr2, frame->rip, wpf)) return;
                }
            }
        }

        // vector 0 at an instruction that is not a divide: an interrupt, see
        // vec0_interrupt_recover. (satoru)
        if (vec0_interrupt_recover(frame)) return;   // any no-error-code vector, see stray_exception_vector (satoru)

        // ---- raw fatal-fault dump, BEFORE any lock/buffer/mirror. ----
        // the pretty dump below goes through SerialLogger (per-cpu line buffer +
        // cross-core lock + RuntimeLog mirror) and g_exc_dump_lock; if the fault
        // interrupted any of that machinery, or the dump path faults again, the
        // report dies with the machine (observed: a boot whose serial ends mid
        // "!!! EXCEPTION: Page Faul" and nothing after - the lottery boots were
        // fatal faults whose reports never made it out). so first push the
        // essentials to the uart with raw outb: no locks, no line buffer, no
        // mirror, bounded tx wait. a nested fault during the dump raw-reports
        // and parks the cpu instead of recursing into a silent triple fault. (satoru)
        static volatile uint8_t s_exc_in_dump[SMP_MAX_CPUS] = {};
        {
            uint32_t mycpu = SMP::CpuIndex();
            if (mycpu >= SMP_MAX_CPUS) mycpu = 0;
            auto raw_putc = [](char ch) {
                for (int t = 0; t < 100000; t++) {
                    if (HAL::InByte(0x3F8 + 5) & 0x20) break;
                }
                HAL::OutByte(0x3F8, (uint8_t)ch);
            };
            auto raw_str = [&](const char* s) { while (*s) raw_putc(*s++); };
            auto raw_hex = [&](uint64_t v) {
                const char* hx = "0123456789ABCDEF";
                raw_str("0x");
                for (int i = 60; i >= 0; i -= 4) raw_putc(hx[(v >> i) & 0xF]);
            };
            if (s_exc_in_dump[mycpu]) {
                // nested fault while dumping: report raw + park, never recurse. (satoru)
                raw_str("\r\n[RAWEXC] NESTED vec="); raw_hex(vec);
                raw_str(" rip="); raw_hex(frame->rip);
                raw_str(" cr2="); raw_hex(frame->cr2);
                raw_str(" cpu="); raw_hex(mycpu);
                raw_str("\r\n");
                // a parked cpu can never ack a tlb ipi - drop it from the online
                // census or every later flush burns its 20ms timeout. and never
                // park on a dead group's cr3 (reaper recycles its tables). (satoru)
                SMP::LoadKernelCr3();
                SMP::MarkSelfOffline();
                for (;;) __asm__ __volatile__("cli; hlt");
            }
            s_exc_in_dump[mycpu] = 1;
            raw_str("\r\n[RAWEXC] vec="); raw_hex(vec);
            raw_str(" rip="); raw_hex(frame->rip);
            raw_str(" err="); raw_hex(frame->error_code);
            if (vec == 14) { raw_str(" cr2="); raw_hex(frame->cr2); }
            raw_str(" cs="); raw_hex(frame->cs);
            raw_str(" rsp="); raw_hex(frame->rsp);
            raw_str(" cpu="); raw_hex(mycpu);
            raw_str("\r\n");
            // torn-frame forensics for the ring-3 crossbeam class: the callee-
            // saved regs + which save path this thread RESUMED from + a window
            // of its user stack. the stack reads go through the page-table walk
            // (identity-mapped phys), NEVER a user-va deref - this raw path
            // must not re-fault. cap 2 dumps per boot. (satoru)
            if ((frame->cs & 3) == 3) {
                static volatile uint32_t s_ff_dumps = 0;
                if (s_ff_dumps < 2) {
                    s_ff_dumps++;
                    raw_str("[RAWEXC2] rbx="); raw_hex(frame->rbx);
                    raw_str(" rbp="); raw_hex(frame->rbp);
                    raw_str(" r15="); raw_hex(frame->r15);
                    raw_str(" rax="); raw_hex(frame->rax);
                    raw_str(" r12="); raw_hex(frame->r12);
                    raw_str(" r13="); raw_hex(frame->r13);
                    raw_str(" r14="); raw_hex(frame->r14);
                    raw_str("\r\n");
                    raw_str("[RAWEXC2] rdi="); raw_hex(frame->rdi);
                    raw_str(" rsi="); raw_hex(frame->rsi);
                    raw_str(" rdx="); raw_hex(frame->rdx);
                    raw_str(" rcx="); raw_hex(frame->rcx);
                    raw_str(" r8="); raw_hex(frame->r8);
                    raw_str(" r9="); raw_hex(frame->r9);
                    raw_str(" r10="); raw_hex(frame->r10);
                    raw_str(" r11="); raw_hex(frame->r11);
                    raw_str("\r\n");
                    Process* ft = Scheduler::GetCurrentProcess();
                    if (ft && ft->is_user()) {
                        // a ring-3 hlt is musl's a_crash assert. dump the words
                        // around the likely offending pointer (rdi) and the two
                        // callee-saved pointers so a corrupted header can be read
                        // back through the page tables on the next boot. (satoru)
                        if (vec == 13) {
                            uint64_t spin[3] = { frame->rdi, frame->rbx, frame->r12 };
                            for (int si = 0; si < 3; si++) {
                                uint64_t c = spin[si];
                                if (c < 0x10000ull || c >= 0x800000000000ull) continue;
                                raw_str("[RAWEXC2] ptr"); raw_hex((uint64_t)si);
                                raw_str("="); raw_hex(c); raw_str(":");
                                for (int wi = -4; wi <= 4; wi++) {
                                    uint64_t va = (c & ~7ULL) + (uint64_t)(wi * 8);
                                    uint64_t ph = KernelVMM::QueryMappingInAddressSpace(
                                        ft->address_space, va & ~0xFFFULL);
                                    raw_str(" ");
                                    if (ph) raw_hex(*(volatile uint64_t*)
                                                    ((uintptr_t)PMM::PhysToVirt(ph & ~0xFFFULL) | (va & 0xFFFULL)));
                                    else raw_str("unmapped");
                                }
                                raw_str("\r\n");
                            }
                        }
                        raw_str("[RAWEXC2] savesite="); raw_hex(ft->last_save_site);
                        raw_str(" saveseq="); raw_hex(ft->save_seq);
                        raw_str(" oncpu="); raw_hex(ft->on_cpu);
                        raw_str(" lastcpu="); raw_hex(ft->last_run_cpu);
                        raw_str("\r\n");
                        // [YRET] decode row: the save-side fingerprint of the
                        // yield-return slot + which save this task last resumed
                        // from (loadseq != saveseq = stale resume). (satoru)
                        raw_str("[RAWEXC2] loadseq="); raw_hex(ft->last_load_seq);
                        raw_str(" yseq="); raw_hex(ft->yret_seq);
                        raw_str(" yva="); raw_hex(ft->yret_va);
                        raw_str(" yval="); raw_hex(ft->yret_val);
                        raw_str(" yphys="); raw_hex(ft->yret_phys);
                        raw_str("\r\n");
                        uint64_t sbase = frame->rsp & ~7ULL;   // a torn rsp may be unaligned (satoru)
                        // wrong-generation probe (rec #5): the PHYS frame
                        // of the fault-rsp page - the join key against kmemx/
                        // quarantine serial events for the lost-dirty-store
                        // stale-restore hypothesis. (satoru)
                        raw_str("[RAWEXC2] rspphys=");
                        raw_hex(KernelVMM::QueryMappingInAddressSpace(
                            ft->address_space, sbase & ~0xFFFULL));
                        raw_str("\r\n");
                        // qi from -16: the crossbeam fault fires INSTANTLY after
                        // the epilogue pops, so [rsp-0x28] (closure saved-r12) and
                        // [rsp-0x70] (wait_until saved-rbp) still hold what was
                        // popped - the one-boot H3 discriminator (rec #1).
                        // if [rsp-0x28]==0xA in MEMORY the spill slot itself was
                        // corrupted; if it holds the correct TLS pointer, the
                        // registers did not come from these pops. (satoru)
                        for (int qi = -16; qi <= 13; qi++) {
                            uint64_t va = sbase + (uint64_t)qi * 8;
                            if ((va & 0xFFFULL) > 0xFF8ULL) continue;   // never straddle a page (satoru)
                            uint64_t ph = KernelVMM::QueryMappingInAddressSpace(
                                ft->address_space, va & ~0xFFFULL);
                            raw_str("[RAWEXC2] stk+"); raw_hex((uint64_t)(qi * 8));
                            raw_str("=");
                            if (ph) raw_hex(*(volatile uint64_t*)(uintptr_t)
                                            ((ph & ~0xFFFULL) | (va & 0xFFFULL)));
                            else raw_str("unmapped");
                            raw_str("\r\n");
                        }
                    }
                }
            }
        }

        // ── CONTAIN A RING-3 FAULT NOW, before the pretty dump. ──────────────
        // the pretty dump below dereferences the FAULTING process's user memory
        // (its tcb + user stack) to diagnose. on a corrupt/mid-teardown address
        // space - exactly the state firefox's jit null-deref leaves - those
        // derefs FAULT AGAIN inside this handler -> #DF -> panic (the reload /
        // search / new-tab reboot). a ring-3 fault must never take the kernel
        // down, and we already have the essential info (the raw [RAWEXC] line
        // above), so handle it here with ZERO further user-memory access. the
        // raw [RAWEXC] already emitted; disarm the nested-dump guard and leave
        // panic-raw off since we are surviving. (satoru)
        if ((frame->cs & 3) == 3) {
            { uint32_t mc = SMP::CpuIndex(); if (mc >= SMP_MAX_CPUS) mc = 0; s_exc_in_dump[mc] = 0; }
            SerialLogger::Log("[usrflt] ring-3 fault contained (no os panic)\r\n");
            // the raw line above only reaches the uart: put the essentials in the disk log too (satoru)
            SerialLogger::Log("[usrflt] vec="); SerialLogger::LogHex64(vec);
            SerialLogger::Log(" rip="); SerialLogger::LogHex64(frame->rip);
            SerialLogger::Log(" cr2="); SerialLogger::LogHex64(frame->cr2);
            SerialLogger::Log(" err="); SerialLogger::LogHex64(frame->error_code);
            SerialLogger::Log("\r\n");
            // symbolized census line (thread, module+offset, stack, MOZ_CRASH
            // reason). kernel tables + phys reads only, no user deref. (satoru)
            LinuxSyscall::DumpUserFault(Scheduler::GetCurrentProcess(), frame->rip, frame->rsp,
                                        vec == 14 ? frame->cr2 : ~0ull);
            // bsp: longjmp back to the RunProcessWithArgs launcher (SIGSEGV
            // semantics). no-op + returns on an ap (no launcher context). (satoru)
            // sched=2 first: on an ap HandleProcessExit would mark the cpu's
            // dispatch-time active_process exited, which after a preempt switch
            // can be a different (or already reaped) task. (satoru)
            if (sched2_contain_ap_fault(frame)) return;   // sched=2: no core lost (satoru)
            Userspace::HandleProcessExit(139);
            // ap fell through: kill the WHOLE faulting address-space group so no
            // sibling resumes the bad jit code on another core, then PARK this
            // cpu. the #PF ISR stub cannot cleanly task-switch (its epilogue
            // restores the faulting regs), so we do NOT try - we just stop this
            // core. the other cores + scheduler keep the os + desktop alive.
            // lightweight kill: remove-from-ready + set-terminated only, no
            // reap/quiesce/sleep - safe here since ring-3 held no kernel lock. (satoru)
            {
                Process* dead = Scheduler::GetCurrentProcess();
                if (dead && dead->is_user()) {
                    Process* grp[SCHED_GROUP_MAX];   // was 64: firefox runs ~80 threads (satoru)
                    int gn = Scheduler::CollectAddressSpaceGroup(dead->pid, grp, SCHED_GROUP_MAX);
                    for (int i = 0; i < gn; i++) Scheduler::MarkProcessExited(grp[i], 139);
                    if (gn == 0) { Scheduler::MarkProcessExited(dead, 139); grp[0] = dead; gn = 1; }
                    Scheduler::SetCurrentForThisCpu(nullptr);
                    // release this task's frame ownership: the normal release
                    // store never runs on a parked cpu, and a stuck on_cpu=1
                    // makes the deferred reaper's off-check fail forever. the
                    // frame is dead (this cpu never resumes it). (satoru)
                    __atomic_store_n(&dead->on_cpu, (uint8_t)0, __ATOMIC_RELEASE);
                    // PIN this thread's kernel stack (leak it): this cpu parks
                    // ON that stack (tss.rsp0), and with current cleared the
                    // reaper sees the group quiesced and would free it - then
                    // an NMI on the parked core pushes into recycled memory,
                    // and the recycler's zeroing smashes whoever owns it next
                    // (the rip=0x3 zeroed-kernel-frame class). 64k per
                    // contained crash, the cpu is lost anyway. (satoru)
                    dead->kernel_stack_top = 0;
                    // do NOT QueueDeferredReap here: this runs INSIDE the #PF
                    // handler on the ap, and taking g_reap_lock + writing the
                    // shared table from that fragile context escalated a
                    // contained crossbeam fault into a #DF -> panic (hg_r2).
                    // the group is marked Terminated, so the futex sweep still
                    // retires its waiter slots (state >= Terminated) - the leak
                    // fix survives; only the struct/address-space reclaim is
                    // skipped (the original, proven-safe leak). single stores
                    // only past this point. (satoru)
                }
                SerialLogger::Log("[usrflt] group killed, cpu parked\r\n");
                // drop from the online census: a parked cpu never acks tlb ipis,
                // and counting it made every later flush time out at 20ms. park
                // on the KERNEL root - the killed group's tables get reaped. (satoru)
                SMP::LoadKernelCr3();
                SMP::MarkSelfOffline();
                for (;;) __asm__ __volatile__("cli; hlt");
            }
        }

        // the whole pretty dump below runs in panic-raw serial mode: direct
        // uart writes, no per-cpu line buffer, no cross-core serial lock, no
        // RuntimeLog mirror. an observed lottery boot wedged the entire box
        // here - this core blocked in the log machinery (irqs off) while a
        // sibling core spun waiting for our tlb-shootdown ack. (satoru)
        SerialLogger::SetPanicRaw(true);
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
        // (satoru) raw kernel-stack dump for a corrupted-control-flow fault: an
        // #UD with a tiny RIP (e.g. RIP=3) is a ret/call to a smashed target;
        // the real call chain is the return addresses still sitting on the stack.
        // -O2 omits frame pointers so an rbp-walk is unreliable -- dump raw qwords
        // from RSP upward (already-used stack, safely mapped) and symbolize the
        // code-looking ones offline with addr2line. ring-0 faults only. debug.
        if ((frame->cs & 3) == 0 && frame->rsp &&
            ((frame->rsp >> 47) == 0 || (frame->rsp >> 47) == 0x1FFFFULL)) {
            const uint64_t* sp = (const uint64_t*)(uintptr_t)frame->rsp;
            // stay on rsp's own page: past it can be the guard page of the next
            // kernel stack up. reading it faulted inside this dump and turned the
            // laptop's #gp into a bogus "kernel stack overflow" panic naming the
            // proc that owns that guard. (satoru)
            int words = (int)((0x1000 - (frame->rsp & 0xFFF)) / 8);
            if (words > 32 || words <= 0) words = 32;
            for (int i = 0; i < words; i++) {
                if ((i & 3) == 0) { SerialLogger::Log("\r\n  S+"); SerialLogger::LogDec(i * 8); SerialLogger::Log(": "); }
                SerialLogger::Log("0x");
                SerialLogger::LogHex((uint32_t)(sp[i] >> 32)); SerialLogger::LogHex((uint32_t)sp[i]);
                SerialLogger::Log(" ");
            }
            SerialLogger::Log("\r\n");
        }
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
                    uint64_t v = ph ? *(volatile uint64_t*)((uintptr_t)PMM::PhysToVirt(ph) + ((va + off) & 0xFFF)) : 0xDEADULL;
                    log_hex(p, v);
                }
            };
            dump_tcb("liveTCB", live_fs);
            if (frame->r14 != live_fs) dump_tcb("r14TCB", frame->r14);
            // user stack qwords at the fault: [rsp+0x18] is the return address
            // for a 3-push prologue - symbolizing it names the CALLER of the
            // faulting function (the AssignLiteral garbage-args hunt). (satoru)
            if (fp) {
                for (uint64_t off = 0; off <= 0x78; off += 8) {
                    uint64_t va = frame->rsp + off;
                    uint64_t pg = va & ~0xFFFULL;
                    uint64_t ph = KernelVMM::QueryMappingInAddressSpace(fp->address_space, pg);
                    if (!ph) continue;
                    char p[16]; int n = 0;
                    const char* tg = "ustk +";
                    while (tg[n]) { p[n] = tg[n]; n++; }
                    const char* hx = "0123456789ABCDEF";
                    p[n++] = hx[(off >> 4) & 0xF]; p[n++] = hx[off & 0xF];
                    p[n] = 0;
                    uint64_t v = *(volatile uint64_t*)((uintptr_t)PMM::PhysToVirt(ph) + (va & 0xFFF));
                    log_hex(p, v);
                }
            }
            // gecko's MOZ_CRASH stores its reason in gMozCrashReason, then writes
            // the source line to address 0. release builds print nothing, so read
            // the reason back here. the crash sequence ending at rip is
            //   mov gMozCrashReason@GOT(%rip),%rcx   48 8b 0d <disp32>
            //   mov %rax,(%rcx)                      48 89 01
            //   xor %eax,%eax                        31 c0
            //   mov $line,%ecx                       b9 <imm32>
            // so the GOT slot is found from the bytes 17 before rip.
            auto user_read = [&](uint64_t va, void* out, int n) -> bool {
                uint8_t* o = (uint8_t*)out;
                for (int i = 0; i < n; i++) {
                    uint64_t a = va + i;
                    uint64_t ph = KernelVMM::QueryMappingInAddressSpace(fp->address_space, a & ~0xFFFULL);
                    if (!ph) return false;
                    o[i] = *(volatile uint8_t*)((uintptr_t)PMM::PhysToVirt(ph) + (a & 0xFFF));
                }
                return true;
            };
            uint8_t code[17];
            if (fp && frame->cr2 < 0x1000 && user_read(frame->rip - 17, code, 17) &&
                code[0] == 0x48 && code[1] == 0x8B && code[2] == 0x0D &&
                code[7] == 0x48 && code[8] == 0x89 && code[9] == 0x01 &&
                code[10] == 0x31 && code[11] == 0xC0 && code[12] == 0xB9) {
                int32_t disp = (int32_t)(code[3] | code[4] << 8 | code[5] << 16 | (uint32_t)code[6] << 24);
                uint32_t line = code[13] | code[14] << 8 | code[15] << 16 | (uint32_t)code[16] << 24;
                uint64_t got = frame->rip - 17 + 7 + (int64_t)disp, var = 0, str = 0;
                char msg[160];
                if (user_read(got, &var, 8) && user_read(var, &str, 8) && str) {
                    int n = 0;
                    while (n < (int)sizeof(msg) - 1 && user_read(str + n, &msg[n], 1) && msg[n]) n++;
                    msg[n] = 0;
                    SerialLogger::Log("  MOZ_CRASH line ");
                    SerialLogger::LogDec((int)line);
                    SerialLogger::Log(": ");
                    SerialLogger::Log(msg);
                    SerialLogger::Log("\r\n");
                }
            }
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
            // the raw-dump nested guard stays armed through the pretty dump so a
            // fault inside it parks with a report; this path survives (longjmp
            // back to the launcher), so disarm for the next fault and restore
            // normal buffered serial. (satoru)
            { uint32_t mc = SMP::CpuIndex(); if (mc >= SMP_MAX_CPUS) mc = 0; s_exc_in_dump[mc] = 0; }
            SerialLogger::SetPanicRaw(false);
            if (sched2_contain_ap_fault(frame)) return;   // sched=2 first, see above (satoru)
            Userspace::HandleProcessExit(139);  // 128 + SIGSEGV; longjmps on the bsp launcher
            // still here = an AP-DISPATCHED user thread faulted: this cpu has no
            // launcher longjmp context. a ring-3 fault must NEVER panic the whole
            // kernel (observed: firefox's jit null-derefs on new-tab - cr2=0,
            // rip in the 0x1800_xxx jit reservation - on cpu1-3). the #PF ISR
            // STUB cannot cleanly switch tasks: its epilogue restores the
            // faulting task's regs, so an earlier attempt to rewrite *frame +
            // iretq into another task DOUBLE-FAULTED (the reboot the user hit).
            // so instead: mark the WHOLE faulting address-space group terminated
            // (lightweight - remove-from-ready + set state, no reap/quiesce/
            // sleep, safe in this isr context since ring-3 held no kernel lock)
            // so no sibling resumes the bad code on another core, then PARK this
            // cpu. we lose one core until reboot but the other cores + scheduler
            // keep the os + desktop alive - vastly better than a panic. the real
            // cure is stopping the jit crash (baseline jit off in firefox.cfg). (satoru)
            {
                Process* dead = Scheduler::GetCurrentProcess();
                if (dead && dead->is_user()) {
                    Process* grp[SCHED_GROUP_MAX];   // was 64: firefox runs ~80 threads (satoru)
                    int gn = Scheduler::CollectAddressSpaceGroup(dead->pid, grp, SCHED_GROUP_MAX);
                    for (int i = 0; i < gn; i++) Scheduler::MarkProcessExited(grp[i], 139);
                    if (gn == 0) { Scheduler::MarkProcessExited(dead, 139); grp[0] = dead; gn = 1; }
                    Scheduler::SetCurrentForThisCpu(nullptr);
                    // frame release + offline census only - NOT QueueDeferredReap
                    // (see the other ap park site: locking a shared table from
                    // this #PF-handler context #DF'd). group stays Terminated ->
                    // futex sweep still retires its slots. single stores. (satoru)
                    __atomic_store_n(&dead->on_cpu, (uint8_t)0, __ATOMIC_RELEASE);
                    // pin (leak) the stack this cpu parks on - see the usrflt
                    // park site. (satoru)
                    dead->kernel_stack_top = 0;
                    SerialLogger::Log("[apfault] ring-3 fault contained: group killed, cpu parked\r\n");
                    // park on the KERNEL root, not the killed group's cr3. (satoru)
                    SMP::LoadKernelCr3();
                    SMP::MarkSelfOffline();
                    for (;;) __asm__ __volatile__("cli; hlt");
                }
            }
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
        TmIrqMark _tm(frame);   // taskmem: interrupt work is kernel's (satoru)
        uint8_t irq = (uint8_t)(vec - 32);

        // spurious irq detection: check pic isr register
        if (irq == 7) {
            // read master pic isr (ocw3: read isr)
            HAL::OutByte(PIC1_CMD, 0x0B);
            if (!(HAL::InByte(PIC1_CMD) & 0x80)) {
                return;  // spurious - no eoi
            }
        }
        if (irq == 15) {
            // read slave pic isr
            HAL::OutByte(PIC2_CMD, 0x0B);
            if (!(HAL::InByte(PIC2_CMD) & 0x80)) {
                // spurious from slave - still must eoi master
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
            hal_sample_busy(frame, 0);   // bsp busy-time sample for the task manager (satoru)
            Scheduler::OnTimerTick(1);
        }

        // call registered handler (if any)
        if (HAL::irq_handlers[irq]) {
            HAL::irq_handlers[irq](frame);
        }

        // the bsp tick from its own lapic timer (SMP::StartBspLapicTick) arrives on
        // vector 32 too: end it at the lapic only (a non-specific 8259 eoi would end
        // whatever the 8259 has in service). a pit irq0 still in flight from the
        // 8259 has no lapic isr bit and gets the 8259 eoi as before. (satoru)
        if (irq == 0 && SMP::BspLapicTickActive() && (SMP::LapicRead(0x110) & 1u)) {
            SMP::LapicWrite(0xB0, 0);
            return;
        }
        // send eoi
        HAL::SendEOI(irq);
        return;
    }

    hal_stray_vector(frame);
}

//  idt initialization - populate all 256 entries from the nasm stub table
void HAL::InitIDT() {
    // clear all entries
    for (int i = 0; i < 256; i++) {
        idt_entries[i] = {};
    }

    // vectors 0-47 come from isr_stub_table (built in isr_stubs.asm)
    for (int i = 0; i < 48; i++) {
        idt_set(i, isr_stub_table[i]);
    }

    // double fault on its own known-good stack (ist2, per-cpu in the tss). with
    // ist=0 a #DF caused by a bad kernel rsp pushes onto that same bad rsp, the
    // handler never runs, and the box silently triple-faults with no report. (satoru)
    idt_set(8, isr_stub_table[8], 2, 0);

    // user-mode syscall trap gate (int 0x80).
    idt_set(0x80, (uint64_t)(uintptr_t)&isr_stub_128, 0, 3);

    // per-AP LAPIC timer (vector 0x40), kernel-only (dpl 0). (satoru)
    idt_set(0x40, (uint64_t)(uintptr_t)&isr_stub_64, 0, 0);

    // tlb-shootdown ipi (vector 0x41): another core changed page tables in an
    // address space this cpu may be running threads of - reload cr3. (satoru)
    idt_set(0x41, (uint64_t)(uintptr_t)&isr_stub_65, 0, 0);

    // sched=2 resched ipi (vector 0x42): only wakes a halted ap out of hlt so
    // its dispatch loop claims the task that just became runnable. (satoru)
    idt_set(0x42, (uint64_t)(uintptr_t)&isr_stub_66, 0, 0);

    // kld broker msi vectors 0x50..0x57, kernel-only (dpl 0). a device granted
    // to a linux-driver container signals here; the broker bumps a doorbell
    // counter and wakes the container. (satoru)
    {
        void (*kld_stubs[8])() = { isr_stub_80, isr_stub_81, isr_stub_82, isr_stub_83,
                                   isr_stub_84, isr_stub_85, isr_stub_86, isr_stub_87 };
        for (int i = 0; i < 8; i++) idt_set(0x50 + i, (uint64_t)(uintptr_t)kld_stubs[i], 0, 0);
    }

    // vectors 48-255: leave as not-present (type_attr = 0).
    // if hardware triggers one, the cpu will fire a #gp which we handle above.
    // (satoru) not any more: the 1112 laptop boot died on exactly that #gp (error
    // code 0x54b = idt vector 0xa9, external event) with cpu0 halted. every vector
    // nothing claimed now gets a stray stub; isr_common_handler logs it, ends it at
    // the lapic / 8259 when it is in service there, and returns. (satoru)
    for (int i = 48; i < 256; i++) {
        if (idt_entries[i].type_attr == 0)
            idt_set(i, (uint64_t)(uintptr_t)(isr_stray_base + 16 * i));
    }

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
    // POLLED (no handlers) - BUT we keep them UNMASKED on purpose: when the cpu
    // is HLT'd in deep idle, a keystroke/mouse-move IRQ wakes it immediately so
    // the polled InputProcess runs and the desktop responds without waiting for
    // the (WHPX-coalesced, ~500ms) timer IRQ. the unhandled IRQ just EOIs; the
    // wake is the point. (satoru)
    EnableIRQ(0);   // pit timer (drives scheduler wakeups)
    EnableIRQ(1);   // keyboard - polled, but wakes the cpu from HLT
    EnableIRQ(12);  // mouse - polled, but wakes the cpu from HLT

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
    // write the CALLING cpu's own tss: each ap loaded its private ap_tss via
    // SetupAPCpuState, so an ap running a user thread must update that one - 
    // writing the bsp's system_tss from an ap would (a) leave the ap's ring-3
    // interrupt stack stale and (b) point the bsp's rsp0 at the ap thread's
    // kernel stack, colliding two cores on one stack. (satoru)
    uint32_t cpu = SMP::CpuIndex();
    if (cpu == 0) {
        system_tss.rsp0 = rsp0;
        system_tss.ist1 = rsp0;
        // legacy global mirror - only meaningful on the bsp. (satoru)
        g_kernel_syscall_rsp = rsp0;
    } else if (cpu < SMP_MAX_CPUS) {
        ap_tss[cpu].rsp0 = rsp0;
        ap_tss[cpu].ist1 = rsp0;
    }

    // this cpu's PerCpu block, which the SYSCALL stub reads via gs:8 after
    // swapgs. (satoru)
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
    // push the tail of the boot log out before the machine loses ram. the 1 s
    // worker may own the log right now, so retry instead of dropping the tail. (satoru)
    {
        uint64_t t0 = Timer::GetRealMs64();
        while (!DiskLog::Flush() && Timer::GetRealMs64() - t0 < 3000u)
            for (volatile uint32_t d = 0; d < 200000; d++) asm volatile("pause");
    }
    while (true) {
        // 0. the acpi fadt reset register. this is the reset linux uses first
        // on modern intel laptops; 0xcf9 alone is not decoded by every
        // chipset when the firmware leaves its reset control locked. (satoru)
        SerialLogger::Log("HAL: trying the ACPI FADT reset register\r\n");
        AcpiReset();
        SerialLogger::Log("HAL: ACPI FADT reset register did not take\r\n");
        // 0b. the efi runtime reset service, when the firmware handed us a
        // system table. a reset that returns is unsupported. (satoru)
        if (BootAcpi::IsEfiBoot()) {
            SerialLogger::Log("HAL: trying EFI ResetSystem\r\n");
            EfiReset(1);   // warm reset (satoru)
            SerialLogger::Log("HAL: EFI ResetSystem did not take\r\n");
        }

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

static uint32_t g_panic_reset_skip = 0;
void HAL::SetPanicResetSkip(uint32_t mask) { g_panic_reset_skip = mask; }

// a real-time pause with interrupts off: the tsc ms clock, bounded by an
// iteration cap in case the tsc is not running. (satoru)
static void panic_reset_delay_ms(uint32_t ms) {
    uint64_t t0 = Timer::GetRealMs64();
    for (uint64_t n = 0; Timer::GetRealMs64() - t0 < ms && n < (uint64_t)ms * 200000ull; n++)
        asm volatile("pause");
}

// a method that faults (an acpi reset register in unmapped mmio) re-enters the
// panic, which calls this again: g_panic_reset_stage skips what was tried. (satoru)
static volatile uint32_t g_panic_reset_stage = 0;

[[noreturn]] void HAL::PanicReset() {
    DisableInterrupts();
    auto step = [](const char* m) {
        SerialLogger::Log(m);
        DiskLog::PanicFlush();   // the last line on disk names the step that hung (satoru)
    };
    if (!(g_panic_reset_skip & 1) && g_panic_reset_stage < 1) {
        g_panic_reset_stage = 1;
        step("HAL: panic reset 1/4: ACPI FADT reset register\r\n");
        AcpiReset();
        panic_reset_delay_ms(500);
        step("HAL: ACPI FADT reset did not take\r\n");
    }
    if (!(g_panic_reset_skip & 2) && g_panic_reset_stage < 2) {
        g_panic_reset_stage = 2;
        step("HAL: panic reset 2/4: 0xCF9 = 0x02 then 0x06\r\n");
        OutByte(0xCF9, 0x02);
        panic_reset_delay_ms(1);
        OutByte(0xCF9, 0x06);
        panic_reset_delay_ms(500);
        step("HAL: 0xCF9 reset did not take\r\n");
    }
    if (!(g_panic_reset_skip & 4) && g_panic_reset_stage < 3) {
        g_panic_reset_stage = 3;
        step("HAL: panic reset 3/4: keyboard controller 0x64 <- 0xFE\r\n");
        for (int tries = 0; tries < 4; tries++) {
            for (uint32_t spin = 0; (InByte(0x64) & 0x02) && spin < 100000; spin++) asm volatile("pause");
            OutByte(0x64, 0xFE);
            panic_reset_delay_ms(100);
        }
        panic_reset_delay_ms(400);
        step("HAL: keyboard controller reset did not take\r\n");
    }
    step("HAL: panic reset 4/4: triple fault\r\n");
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) null_idt = {0, 0};
    asm volatile("lidt %0" : : "m"(null_idt));
    asm volatile("int3");
    for (;;) asm volatile("cli; hlt");
}

void HAL::PowerOff() {
    DisableInterrupts();
    SerialLogger::Log("HAL: PowerOff requested\r\n");
    {
        uint64_t t0 = Timer::GetRealMs64();
        while (!DiskLog::Flush() && Timer::GetRealMs64() - t0 < 3000u)
            for (volatile uint32_t d = 0; d < 200000; d++) asm volatile("pause");
    }

    // 1. acpi s5 soft off via the fadt pm1 control block and the \_S5 sleep
    // type. this is the real-hardware power-off path; the emulator ports
    // below exist for machines whose tables are missing. (satoru)
    if (AcpiPowerOff()) {
        SerialLogger::Log("HAL: ACPI S5 write did not take\r\n");
    } else {
        SerialLogger::Log("HAL: ACPI S5 unavailable, trying the emulator ports\r\n");
    }

    // 2. try the common emulator/acpi poweroff ports in order. each is a
    // 16-bit write of the sleep value to a hardware-reduced acpi pm1a-style
    // control port that the hypervisor watches. on real hardware these
    // usually do nothing, so we fall through to the halt. (satoru)
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
    SerialLogger::Log("HAL: PowerOff ports had no effect - halting CPU\r\n");
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
    // bootloader-provided rsdp first (uefi has no legacy one). (satoru)
    if (uint64_t br = BootAcpi::Rsdp()) return (uintptr_t)br;
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
                        // entries are not guaranteed 8-byte aligned - copy.
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

// locate the dsdt from the fadt: DSDT (32-bit) at offset 40, or X_DSDT (64-bit)
// at offset 140 on a long-enough fadt. returns 0 if neither resolves. (satoru)
static uintptr_t acpi_find_dsdt(uintptr_t fadt) {
    if (!fadt) return 0;
    const AcpiSDTHeader* fh = (const AcpiSDTHeader*)fadt;
    uint32_t dsdt32 = *(const uint32_t*)(fadt + 40);
    if (dsdt32) return (uintptr_t)dsdt32;
    if (fh->length >= 148) {
        uint64_t x_dsdt = 0;
        const uint8_t* src = (const uint8_t*)(fadt + 140);
        uint8_t* dst = (uint8_t*)&x_dsdt;
        for (int b = 0; b < 8; b++) dst[b] = src[b];
        if (x_dsdt) return (uintptr_t)x_dsdt;
    }
    return 0;
}

// read one small AML integer element from an _Sx package: BytePrefix(0x0A)+byte,
// ZeroOp(0x00), OneOp(0x01), or a bare byte. advances *pp past it. (satoru)
static uint8_t aml_read_small_int(const uint8_t** pp) {
    const uint8_t* p = *pp;
    uint8_t op = *p++;
    uint8_t v;
    if      (op == 0x0A) { v = *p++; }   // BytePrefix (satoru)
    else if (op == 0x00) { v = 0; }      // ZeroOp (satoru)
    else if (op == 0x01) { v = 1; }      // OneOp (satoru)
    else                 { v = op; }     // already the raw value (satoru)
    *pp = p;
    return v;
}

// scan the dsdt aml for Name(_Sx, Package(){SLP_TYPa, SLP_TYPb, ...}) and extract
// the two sleep-type values. this is the ACPICA-free pattern hobby kernels use:
// no full aml interpreter, just a byte scan for the "_Sx_" NameOp package. returns
// true and fills *a/*b on success. (satoru)
static bool acpi_parse_sleep_pkg(uintptr_t dsdt, char which, uint8_t* a, uint8_t* b) {
    if (!dsdt) return false;
    const AcpiSDTHeader* dh = (const AcpiSDTHeader*)dsdt;
    if (!acpi_sig_eq(dh->signature, "DSDT")) return false;
    uint32_t len = dh->length;
    if (len < sizeof(AcpiSDTHeader) + 8) return false;
    const uint8_t* start = (const uint8_t*)(dsdt + sizeof(AcpiSDTHeader));
    const uint8_t* end   = (const uint8_t*)(dsdt + len);
    for (const uint8_t* p = start; p + 8 < end; p++) {
        if (!(p[0] == '_' && p[1] == 'S' && p[2] == which && p[3] == '_')) continue;
        // must be a NameOp (0x08), optionally with a RootChar '\' before the name.
        // (satoru)
        bool is_name = (p > start && p[-1] == 0x08) ||
                       (p > start + 1 && p[-1] == 0x5C && p[-2] == 0x08);
        if (!is_name) continue;
        const uint8_t* pkg = p + 4;
        if (*pkg != 0x12) continue;          // PackageOp (satoru)
        pkg++;
        uint8_t lead = *pkg;                  // PkgLength encoding (satoru)
        int nfollow = lead >> 6;
        pkg += 1 + nfollow;                   // skip PkgLength bytes (satoru)
        pkg++;                                // skip NumElements (satoru)
        // two small-ints can consume up to 4 bytes (two BytePrefix pairs), so
        // require all 4 in range - pkg+2 let a byteprefix pair read 1 byte past
        // the dsdt end. (satoru)
        if (pkg + 4 > end) return false;
        *a = aml_read_small_int(&pkg);        // SLP_TYPa (satoru)
        *b = aml_read_small_int(&pkg);        // SLP_TYPb (satoru)
        return true;
    }
    return false;
}

// generic address structure (acpi 2.0+): where a register lives and how wide
// it is. (satoru)
struct AcpiGas {
    uint8_t  space;        // 0 memory, 1 io, 2 pci config (satoru)
    uint8_t  bit_width;
    uint8_t  bit_offset;
    uint8_t  access;       // 1 byte, 2 word, 3 dword, 4 qword (satoru)
    uint64_t address;
} __attribute__((packed));

// the 32-bit pm1a/b control blocks are at fadt+64/68; when both are zero the
// x_ variants at fadt+168/180 carry the io ports on a long fadt. (satoru)
static void acpi_pm1_cnt_ports(uintptr_t fadt, uint16_t* a, uint16_t* b) {
    *a = 0; *b = 0;
    if (!fadt) return;
    const AcpiSDTHeader* fh = (const AcpiSDTHeader*)fadt;
    uint32_t a32 = *(const uint32_t*)(fadt + 64);
    uint32_t b32 = *(const uint32_t*)(fadt + 68);
    if (a32) *a = (uint16_t)a32;
    if (b32) *b = (uint16_t)b32;
    if (!*a && fh->length >= 180) {
        const AcpiGas* ga = (const AcpiGas*)(fadt + 168);
        const AcpiGas* gb = (const AcpiGas*)(fadt + 180);
        if (ga->space == 1 && ga->address && ga->address <= 0xFFFF) *a = (uint16_t)ga->address;
        if (gb->space == 1 && gb->address && gb->address <= 0xFFFF) *b = (uint16_t)gb->address;
    }
}

// pm1 event block: status at port, enable at port+len/2. the 32-bit port is at
// fadt+56; x_pm1a_evt_blk (fadt+144) is the fallback. (satoru)
static uint16_t acpi_pm1_evt_port(uintptr_t fadt, uint8_t* evt_len) {
    if (evt_len) *evt_len = 4;
    if (!fadt) return 0;
    const AcpiSDTHeader* fh = (const AcpiSDTHeader*)fadt;
    uint32_t p32 = *(const uint32_t*)(fadt + 56);
    uint16_t port = p32 ? (uint16_t)p32 : 0;
    if (!port && fh->length >= 156) {
        const AcpiGas* g = (const AcpiGas*)(fadt + 144);
        if (g->space == 1 && g->address && g->address <= 0xFFFF) port = (uint16_t)g->address;
    }
    uint8_t len = *(const uint8_t*)(fadt + 88);
    if (evt_len && len) *evt_len = len;
    return port;
}

// the fadt reset register (gas at 112) + reset value (byte at 124). only
// present on a fadt long enough to carry both. (satoru)
static bool acpi_reset_register(uintptr_t fadt, AcpiGas* gas, uint8_t* value) {
    if (!fadt) return false;
    const AcpiSDTHeader* fh = (const AcpiSDTHeader*)fadt;
    // RESET_REG is the 12-byte gas at offset 116 and RESET_VALUE the byte at 128
    // (acpi 2.0+, table 5-34). offset 112 is the 32-bit Flags field: read as a
    // gas it never looked valid, so every panic reset skipped the acpi reset and
    // the laptop log ended at "no ACPI reset register in the FADT". (satoru)
    if (fh->length < 129) return false;
    const AcpiGas* g = (const AcpiGas*)(fadt + 116);
    if (!g->address || (g->space != 0 && g->space != 1)) return false;
    if (gas) *gas = *g;
    if (value) *value = *(const uint8_t*)(fadt + 128);
    return true;
}

// write one register through a gas descriptor. (satoru)
static bool acpi_gas_write(const AcpiGas& g, uint8_t value) {
    uint8_t width = g.access;
    if (!width) width = (g.bit_width ? (uint8_t)((g.bit_width + 7) / 8) : 1);
    if (g.space == 1) {
        if (g.address > 0xFFFF) return false;
        if (width >= 4)      HAL::OutLong((uint16_t)g.address, value);
        else if (width == 2) HAL::OutWord((uint16_t)g.address, (uint16_t)value);
        else                 HAL::OutByte((uint16_t)g.address, value);
        return true;
    }
    if (g.space == 0) {
        volatile uint8_t* p = (volatile uint8_t*)(uintptr_t)g.address;
        if (width >= 4)      *(volatile uint32_t*)p = value;
        else if (width == 2) *(volatile uint16_t*)p = (uint16_t)value;
        else                 *p = value;
        return true;
    }
    return false;
}
}  // namespace

bool HAL::Suspend() {
    SerialLogger::Log("HAL: Suspend (ACPI S3) requested\r\n");

    // step 1 (implemented): recover pm1a_cnt_blk from the fadt. the fadt
    // (signature "FACP") stores pm1a_cnt_blk as a 32-bit io port at offset
    // 64 per the acpi spec. (satoru)
    uintptr_t fadt = acpi_find_fadt();
    if (!fadt) {
        SerialLogger::Log("HAL: Suspend aborted - no ACPI RSDP/FADT found\r\n");
        return false;
    }
    uint16_t pm1a_cnt = 0, pm1b_cnt = 0;   // 0 if the platform has no PM1b (satoru)
    acpi_pm1_cnt_ports(fadt, &pm1a_cnt, &pm1b_cnt);
    log_hex("HAL: FADT located, PM1a_CNT_BLK = ", pm1a_cnt);

    // step 2 (implemented): the s3 sleep type (slp_typa/b, bits 10-12 of the pm1
    // control register) is not in the fadt - it lives in the dsdt as the aml
    // object \_S3, e.g. Name(_S3, Package(){SLP_TYPa, SLP_TYPb, 0, 0}). we scan
    // the dsdt bytes for that "_S3_" NameOp package and pull the two bytes out
    // (the ACPICA-free path hobby kernels use); no full aml interpreter. under
    // qemu these values are the well-known SLP_TYP=1. (satoru)
    uintptr_t dsdt = acpi_find_dsdt(fadt);
    if (!dsdt) {
        SerialLogger::Log("HAL: Suspend aborted - no DSDT reachable from the FADT\r\n");
        return false;
    }
    uint8_t slp_typa = 0, slp_typb = 0;
    if (!acpi_parse_sleep_pkg(dsdt, '3', &slp_typa, &slp_typb)) {
        SerialLogger::Log("HAL: Suspend aborted - no \\_S3 package found in the DSDT\r\n");
        return false;
    }
    if (!pm1a_cnt) {
        SerialLogger::Log("HAL: Suspend aborted - FADT PM1a_CNT_BLK is null\r\n");
        return false;
    }
    log_hex("HAL: S3 SLP_TYPa = ", slp_typa);
    log_hex("HAL: S3 SLP_TYPb = ", slp_typb);

    // step 3: quiesce (best effort) and enter S3 by writing (SLP_TYP << 10) |
    // SLP_EN to the pm1 control register(s). SLP_EN is bit 13. we mask irqs first;
    // a FULL device quiesce (stopping DMA engines, saving controller state) and a
    // real resume-context restore are out of scope - on this kernel S3 wake is a
    // FRESH BOOT (the firmware re-enters the bootloader), which is honest for a
    // suspend that has no saved-state resume path yet. (satoru)
    const uint16_t SLP_EN = (uint16_t)(1u << 13);
    uint16_t val_a = (uint16_t)(((uint32_t)(slp_typa & 0x7) << 10) | SLP_EN);
    SerialLogger::Log("HAL: entering S3 (wake = fresh boot; no resume-context restore)\r\n");
    DisableInterrupts();
    OutWord((uint16_t)pm1a_cnt, val_a);
    if (pm1b_cnt) {
        uint16_t val_b = (uint16_t)(((uint32_t)(slp_typb & 0x7) << 10) | SLP_EN);
        OutWord((uint16_t)pm1b_cnt, val_b);
    }
    // spin briefly to give the transition time to take; if we are still executing
    // after that, the sleep did not happen (e.g. the platform rejected it). (satoru)
    for (volatile int i = 0; i < 1000000; i++) __asm__ __volatile__("pause");
    EnableInterrupts();
    SerialLogger::Log("HAL: Suspend - S3 transition did not take (platform rejected SLP_EN)\r\n");
    return false;
}

bool HAL::AcpiReset() {
    AcpiGas gas;
    uint8_t value = 0;
    if (!acpi_reset_register(acpi_find_fadt(), &gas, &value)) {
        SerialLogger::Log("HAL: no ACPI reset register in the FADT\r\n");
        return false;
    }
    log_hex("HAL: ACPI reset register ", gas.address);
    log_hex("HAL: ACPI reset value ", value);
    if (!acpi_gas_write(gas, value)) {
        SerialLogger::Log("HAL: ACPI reset register space is unsupported\r\n");
        return false;
    }
    // give the chipset time to reset; if we are still executing it did not. (satoru)
    for (volatile uint32_t d = 0; d < 2000000; d++) asm volatile("pause");
    return false;
}

bool HAL::AcpiPowerOff() {
    uintptr_t fadt = acpi_find_fadt();
    if (!fadt) return false;
    uint16_t pm1a = 0, pm1b = 0;
    acpi_pm1_cnt_ports(fadt, &pm1a, &pm1b);
    if (!pm1a) {
        SerialLogger::Log("HAL: PowerOff - FADT has no PM1a_CNT_BLK\r\n");
        return false;
    }
    uintptr_t dsdt = acpi_find_dsdt(fadt);
    uint8_t slp_typa = 0, slp_typb = 0;
    if (!acpi_parse_sleep_pkg(dsdt, '5', &slp_typa, &slp_typb)) {
        SerialLogger::Log("HAL: PowerOff - no \\_S5 package found in the DSDT\r\n");
        return false;
    }
    const uint16_t SLP_EN = (uint16_t)(1u << 13);
    log_hex("HAL: S5 SLP_TYPa = ", slp_typa);
    SerialLogger::Log("HAL: entering ACPI S5\r\n");
    OutWord(pm1a, (uint16_t)(((uint32_t)(slp_typa & 0x7) << 10) | SLP_EN));
    if (pm1b) OutWord(pm1b, (uint16_t)(((uint32_t)(slp_typb & 0x7) << 10) | SLP_EN));
    for (volatile uint32_t d = 0; d < 2000000; d++) asm volatile("pause");
    return true;
}

// the efi runtime reset entry takes the microsoft x64 calling convention, not
// the sysv one this kernel is built with. (satoru)
typedef void (__attribute__((ms_abi)) *EfiResetSystemFn)(uint32_t, uint64_t, uint64_t, void*);

bool HAL::EfiReset(uint32_t type) {
    if (!BootAcpi::IsEfiBoot()) return false;
    uint64_t st = BootAcpi::SystemTable();
    if (!st || (st & 7) || st >= (512ull << 30)) return false;
    if (*(const uint64_t*)(uintptr_t)st != 0x5453595320494249ull) return false;   // "IBI SYST"
    uint64_t rt = *(const uint64_t*)(uintptr_t)(st + 0x58);
    if (!rt || (rt & 7) || rt >= (512ull << 30)) return false;
    if (*(const uint64_t*)(uintptr_t)rt != 0x56524553544E5552ull) return false;   // "RUNTSERV"
    uint64_t fn = *(const uint64_t*)(uintptr_t)(rt + 0x68);
    if (!fn || (fn & 7) || fn >= (512ull << 30)) return false;
    log_hex("HAL: EFI ResetSystem type ", type);
    ((EfiResetSystemFn)(uintptr_t)fn)(type, 0, 0, nullptr);
    return false;   // a reset that returns is unsupported
}

// pm1 status/enable port remembered by the power button poller. (satoru)
static uint16_t g_pwrbtn_port = 0;
static uint8_t  g_pwrbtn_len  = 0;

bool HAL::AcpiPowerButtonInit() {
    uintptr_t fadt = acpi_find_fadt();
    if (!fadt) {
        SerialLogger::Log("HAL: power button - no ACPI FADT\r\n");
        return false;
    }
    uint8_t len = 0;
    uint16_t port = acpi_pm1_evt_port(fadt, &len);
    if (!port || len < 4) {
        SerialLogger::Log("HAL: power button - FADT has no PM1 event block\r\n");
        return false;
    }
    uint16_t en_port = (uint16_t)(port + len / 2);
    uint16_t en = InWord(en_port);
    en |= (uint16_t)(1u << 8);          // PWRBTN_EN (satoru)
    OutWord(en_port, en);
    uint16_t st = InWord(port);
    if (st & (1u << 8)) OutWord(port, (uint16_t)(1u << 8));   // clear a stale press (satoru)
    g_pwrbtn_port = port;
    g_pwrbtn_len = len;
    log_hex("HAL: ACPI power button armed, PM1_EVT_BLK = ", port);
    log_hex("HAL: pwrbtn enable = ", InWord(en_port));
    log_hex("HAL: pwrbtn status = ", InWord(port));
    return true;
}

bool HAL::AcpiPowerButtonPressed() {
    if (!g_pwrbtn_port) return false;
    uint16_t st = InWord(g_pwrbtn_port);
    if (!(st & (1u << 8))) return false;
    OutWord(g_pwrbtn_port, (uint16_t)(1u << 8));   // write 1 to clear (satoru)
    (void)g_pwrbtn_len;
    return true;
}

uint16_t HAL::AcpiPowerButtonStatus() {
    return g_pwrbtn_port ? InWord(g_pwrbtn_port) : 0;
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
    // write to unused port 0x80 - introduces ~1 µs delay for pic/slow devices
    asm volatile("outb %%al, $0x80" : : "a"((uint8_t)0));
}

void HAL::MemoryBarrier() {
    asm volatile("mfence" : : : "memory");
}

// ── [ubchk] user-boundary trace (corruptor hunt) ─────────────────────────────
// a per-thread ring of every ring-3 entry/exit the kernel sees (interrupt or
// exception in/out, syscall in/out, SaveUserFrame, LoadUserFrame) with rip and
// rsp, in a side table keyed by the Process pointer (struct Process must not
// grow). the check: an exit that resumes a thread at the SAME rip it entered at
// must also resume it with the same rsp and callee-saved registers; nothing
// legitimate changes rsp without changing rip (signal delivery, sigreturn,
// restart and execve all move rip). a mismatch means the kernel handed the
// thread a register state it did not save. kurono.ubtrace=1. (satoru)
bool g_ubtrace_on = false;
namespace {
constexpr int UB_SLOTS = 4096;
constexpr int UB_EV = 24;
struct UbEv { uint64_t rip, rsp; uint32_t tsc; uint8_t kind, cpu; uint16_t code; };
struct UbSlot {
    const Process* owner; uint32_t pid; uint32_t head;
    uint64_t in_rip, in_rsp, in_csum; uint8_t in_valid, in_kind; uint16_t in_code;
    UbEv ev[UB_EV];
};
UbSlot g_ub[UB_SLOTS];
volatile uint32_t g_ub_alerts = 0;
inline uint64_t ub_csum(const InterruptFrame* f) {
    return f->rbx ^ (f->rbp * 0x9E3779B97F4A7C15ull) ^ (f->r12 << 1) ^ (f->r13 >> 1)
         ^ (f->r14 * 3) ^ (f->r15 + 0xD1B54A32D192ED03ull);
}
// Process objects are page-granular heap blocks, so the low pointer bits are
// nearly constant: mix the whole pointer (a plain >>4 put most threads in a
// few slots and turned the [ubchk] check into cross-thread noise). (satoru)
inline uint32_t ub_hash(const Process* p) {
    uint64_t x = (uint64_t)(uintptr_t)p * 0x9E3779B97F4A7C15ull;
    return (uint32_t)(x >> 52) % UB_SLOTS;
}
inline UbSlot* ub_slot(const Process* p) {
    if (!p) return nullptr;
    UbSlot* s = &g_ub[ub_hash(p)];
    if (s->owner != p || s->pid != p->pid) {
        s->owner = p; s->pid = p->pid; s->head = 0; s->in_valid = 0;
        for (int i = 0; i < UB_EV; i++) s->ev[i].kind = 0;
    }
    return s;
}
inline void ub_push(UbSlot* s, uint8_t kind, uint16_t code, uint64_t rip, uint64_t rsp) {
    uint32_t lo, hi; __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    UbEv& e = s->ev[s->head % UB_EV];
    e.rip = rip; e.rsp = rsp; e.tsc = (uint32_t)((((uint64_t)hi << 32) | lo) >> 10);
    e.kind = kind; e.cpu = (uint8_t)SMP::CpuIndex(); e.code = code;
    s->head++;
}
const char* const ub_kn[] = { "-", "IN", "OUT", "SYS", "SRET", "SAVE", "LOAD", "SIG" };
}  // namespace

void UbDump(const Process* p) {
    if (!g_ubtrace_on || !p) return;
    UbSlot* s = &g_ub[ub_hash(p)];
    if (s->owner != p) { SerialLogger::Log("[ubtr] no trace for this thread\r\n"); return; }
    SerialLogger::Log("[ubtr] pid="); SerialLogger::LogDec((int)p->pid);
    SerialLogger::Log(" last events, oldest first (kind code cpu tsc/1k rip rsp):\r\n");
    uint32_t n = s->head < (uint32_t)UB_EV ? s->head : (uint32_t)UB_EV;
    for (uint32_t i = s->head - n; i != s->head; i++) {
        const UbEv& e = s->ev[i % UB_EV];
        if (!e.kind) continue;
        SerialLogger::Log("[ubtr]  "); SerialLogger::Log(e.kind < 8 ? ub_kn[e.kind] : "?");
        SerialLogger::Log(" "); SerialLogger::LogHex64(e.code);
        SerialLogger::Log(" c"); SerialLogger::LogDec(e.cpu);
        SerialLogger::Log(" t"); SerialLogger::LogDec((int)e.tsc);
        SerialLogger::Log(" rip="); SerialLogger::LogHex64(e.rip);
        SerialLogger::Log(" rsp="); SerialLogger::LogHex64(e.rsp);
        SerialLogger::Log("\r\n");
    }
}

static void ub_in(const InterruptFrame* f, uint8_t kind, uint16_t code) {
    if ((f->cs & 3) != 3) return;
    Process* p = Scheduler::GetCurrentProcess();
    if (!p || !p->is_user()) return;
    UbSlot* s = ub_slot(p);
    ub_push(s, kind, code, f->rip, f->rsp);
    s->in_rip = f->rip; s->in_rsp = f->rsp; s->in_csum = ub_csum(f);
    s->in_valid = 1; s->in_kind = kind; s->in_code = code;
}

static void ub_out(const InterruptFrame* f, uint8_t kind) {
    if ((f->cs & 3) != 3) return;
    Process* p = Scheduler::GetCurrentProcess();
    if (!p || !p->is_user()) return;
    UbSlot* s = ub_slot(p);
    ub_push(s, kind, 0, f->rip, f->rsp);
    if (s->in_valid && f->rip == s->in_rip &&
        (f->rsp != s->in_rsp || ub_csum(f) != s->in_csum)) {
        uint32_t k = __atomic_fetch_add(&g_ub_alerts, 1, __ATOMIC_RELAXED);
        if (k < 6) {
            SerialLogger::Log("[ubchk] MISMATCH pid="); SerialLogger::LogDec((int)p->pid);
            SerialLogger::Log(" thr="); SerialLogger::Log(p->name);
            SerialLogger::Log(" cpu="); SerialLogger::LogDec((int)SMP::CpuIndex());
            SerialLogger::Log(" rip="); SerialLogger::LogHex64(f->rip);
            SerialLogger::Log(" in_rsp="); SerialLogger::LogHex64(s->in_rsp);
            SerialLogger::Log(" out_rsp="); SerialLogger::LogHex64(f->rsp);
            SerialLogger::Log(" csum="); SerialLogger::Log(ub_csum(f) == s->in_csum ? "same" : "DIFF");
            SerialLogger::Log(" in="); SerialLogger::Log(ub_kn[s->in_kind]);
            SerialLogger::Log("/"); SerialLogger::LogHex64(s->in_code);
            SerialLogger::Log(" rbx="); SerialLogger::LogHex64(f->rbx);
            SerialLogger::Log(" rbp="); SerialLogger::LogHex64(f->rbp);
            SerialLogger::Log("\r\n");
            UbDump(p);
        }
    }
    s->in_valid = 0;
}

void UbNoteSave(const Process* p, const InterruptFrame* f, uint8_t site) {
    if (!g_ubtrace_on || !p || !f) return;
    UbSlot* s = ub_slot(p);
    ub_push(s, 5, site, f->rip, f->rsp);
}
void UbNoteLoad(const Process* p, const InterruptFrame* f) {
    if (!g_ubtrace_on || !p || !f) return;
    UbSlot* s = ub_slot(p);
    ub_push(s, 6, 0, f->rip, f->rsp);
}
void UbNoteSignal(const Process* p, uint64_t rip, uint64_t rsp, int sig) {
    if (!g_ubtrace_on || !p) return;
    UbSlot* s = ub_slot(p);
    ub_push(s, 7, (uint16_t)sig, rip, rsp);
    s->in_valid = 0;
}

extern "C" void ub_trace_isr_exit(InterruptFrame* f) {
    if (g_ubtrace_on) ub_out(f, 2);
}
extern "C" void ub_trace_sys_in(InterruptFrame* f) {
    if (g_ubtrace_on) ub_in(f, 3, (uint16_t)f->rax);
}
extern "C" void ub_trace_sys_out(InterruptFrame* f) {
    if (g_ubtrace_on) ub_out(f, 4);
}
void UbTraceIsrIn(InterruptFrame* f) {
    if (g_ubtrace_on) ub_in(f, 1, (uint16_t)f->vector);
}
// end [ubchk] (satoru)
