#include "smp.h"
#include "../drivers/serial.h"
#include "../drivers/cpu_detect.h"
#include "../kernel/pmm.h"
#include "../hal/hal.h"
#include "scheduler.h"            // ap dispatch: claim + run user procs (smp 3d) (satoru)
#include "spinlock.h"
#include "../kernel/userspace.h"  // Userspace::RunProcessWithArgs on the ap (satoru)
#include "../linux/linux_syscall.h" // drain the ap process's console output (satoru)

//  the ap trampoline blob (flat binary, embedded via objcopy)  -  copied to phys
//  0x8000 before the first SIPI. (satoru)
extern "C" uint8_t _binary_ap_trampoline_bin_start[];
extern "C" uint8_t _binary_ap_trampoline_bin_end[];

//  switch_to.asm: pivot onto a saved InterruptFrame and iretq into ring-3  - 
//  how an ap RESUMES a claimed sibling thread. never returns. (satoru)
extern "C" [[noreturn]] void ap_enter_user_frame(InterruptFrame* f);

//  the ap dispatch loop (defined below ap_entry)  -  also the iret target the
//  syscall exit path uses when an ap runs out of threads. (satoru)
extern "C" [[noreturn]] void ap_dispatch_reenter();

//  smp phase 1  -  lapic enable + cpu enumeration + per-cpu blocks. see smp.h. (satoru)

namespace {
    //  per-cpu blocks + the apic-id -> dense-index map (apic ids can be sparse). (satoru)
    PerCpu   g_cpus[SMP_MAX_CPUS];
    int      g_apicid_to_index[256];
    uint32_t g_cpu_count = 0;
    uint64_t g_lapic_base = 0;     // identity-mapped xapic mmio window (satoru)
    bool     g_inited = false;

    //  phase 3d gate + a lock so two cores' ap-dispatch serial lines don't interleave. (satoru)
    volatile bool g_ap_user_sched = false;
    Spinlock      g_ap_log_lock;

    //  thread-dispatch gate (kurono.apthreads): aps also resume ready sibling
    //  threads, giving a multi-threaded process true parallelism. (satoru)
    volatile bool g_ap_thread_sched = false;

    //  tlb-shootdown ack counter: receivers increment after reloading cr3. (satoru)
    volatile uint32_t g_tlb_ack = 0;

    inline uint64_t rdmsr(uint32_t msr) {
        uint32_t lo, hi;
        __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
        return ((uint64_t)hi << 32) | lo;
    }

    inline uint64_t rdtsc() {
        uint32_t lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        return ((uint64_t)hi << 32) | lo;
    }

    inline void wrmsr(uint32_t msr, uint64_t val) {
        uint32_t lo = (uint32_t)val, hi = (uint32_t)(val >> 32);
        __asm__ volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(msr));
    }

    const uint32_t MSR_KERNEL_GS_BASE = 0xC0000102u;

    //  busy-wait  -  interrupts are disabled at this point in boot so we cannot
    //  sleep; spin on the (already-calibrated) tsc instead. (satoru)
    void busy_us(uint64_t us) {
        uint64_t hz = (uint64_t)CPUDetect::GetInfo().frequency.tsc_frequency;
        if (hz < 1000000ULL) hz = 2000000000ULL;   // ~2ghz fallback (satoru)
        uint64_t ticks = (hz / 1000000ULL) * us;
        uint64_t start = rdtsc();
        while (rdtsc() - start < ticks) __asm__ volatile("pause");
    }

    inline uint32_t mmio_r32(uint64_t addr) {
        return *(volatile uint32_t*)(uintptr_t)addr;
    }
    inline void mmio_w32(uint64_t addr, uint32_t val) {
        *(volatile uint32_t*)(uintptr_t)addr = val;
    }

    bool sig4(const uint8_t* p, const char* s) {
        return p[0] == (uint8_t)s[0] && p[1] == (uint8_t)s[1] &&
               p[2] == (uint8_t)s[2] && p[3] == (uint8_t)s[3];
    }
    bool sig8(const uint8_t* p, const char* s) {
        for (int i = 0; i < 8; i++) if (p[i] != (uint8_t)s[i]) return false;
        return true;
    }
    uint8_t checksum(const uint8_t* p, uint32_t n) {
        uint8_t s = 0;
        for (uint32_t i = 0; i < n; i++) s = (uint8_t)(s + p[i]);
        return s;
    }

    //  acpi system description table header (36 bytes). (satoru)
    struct AcpiSdt {
        uint8_t  signature[4];
        uint32_t length;
        uint8_t  revision;
        uint8_t  checksum;
        uint8_t  oem_id[6];
        uint8_t  oem_table_id[8];
        uint32_t oem_revision;
        uint32_t creator_id;
        uint32_t creator_revision;
    } __attribute__((packed));

    //  scan the legacy bios area (+ ebda) for the "RSD PTR " anchor. returns the
    //  physical address of the rsdp, or 0. all of this is identity-mapped. (satoru)
    uint64_t find_rsdp() {
        // ebda first (segment word at 0x40e, shifted left 4). (satoru)
        uint32_t ebda = ((uint32_t)*(volatile uint16_t*)(uintptr_t)0x40E) << 4;
        if (ebda >= 0x400 && ebda < 0xA0000) {
            for (uint32_t a = ebda; a < ebda + 1024; a += 16)
                if (sig8((const uint8_t*)(uintptr_t)a, "RSD PTR ") &&
                    checksum((const uint8_t*)(uintptr_t)a, 20) == 0)
                    return a;
        }
        for (uint32_t a = 0xE0000; a < 0x100000; a += 16)
            if (sig8((const uint8_t*)(uintptr_t)a, "RSD PTR ") &&
                checksum((const uint8_t*)(uintptr_t)a, 20) == 0)
                return a;
        return 0;
    }

    //  locate the madt ("APIC") via the rsdt (acpi 1.0) or xsdt (acpi 2.0+). (satoru)
    const AcpiSdt* find_madt(uint64_t rsdp_phys) {
        const uint8_t* rsdp = (const uint8_t*)(uintptr_t)rsdp_phys;
        uint8_t revision = rsdp[15];
        if (revision >= 2) {
            uint64_t xsdt = *(const uint64_t*)(rsdp + 24);
            const AcpiSdt* x = (const AcpiSdt*)(uintptr_t)xsdt;
            if (!x) return nullptr;
            uint32_t n = (x->length - sizeof(AcpiSdt)) / 8;
            const uint64_t* ptrs = (const uint64_t*)((const uint8_t*)x + sizeof(AcpiSdt));
            for (uint32_t i = 0; i < n; i++) {
                const AcpiSdt* t = (const AcpiSdt*)(uintptr_t)ptrs[i];
                if (t && sig4(t->signature, "APIC")) return t;
            }
        } else {
            uint32_t rsdt = *(const uint32_t*)(rsdp + 16);
            const AcpiSdt* r = (const AcpiSdt*)(uintptr_t)rsdt;
            if (!r) return nullptr;
            uint32_t n = (r->length - sizeof(AcpiSdt)) / 4;
            const uint32_t* ptrs = (const uint32_t*)((const uint8_t*)r + sizeof(AcpiSdt));
            for (uint32_t i = 0; i < n; i++) {
                const AcpiSdt* t = (const AcpiSdt*)(uintptr_t)ptrs[i];
                if (t && sig4(t->signature, "APIC")) return t;
            }
        }
        return nullptr;
    }

    void register_cpu(uint32_t apic_id) {
        if (g_cpu_count >= SMP_MAX_CPUS) return;
        if (apic_id < 256 && g_apicid_to_index[apic_id] >= 0) return;  // dup (satoru)
        uint32_t idx = g_cpu_count++;
        g_cpus[idx].user_rsp_save = 0;
        g_cpus[idx].kernel_rsp = 0;
        g_cpus[idx].cpu_index = idx;
        g_cpus[idx].apic_id = apic_id;
        g_cpus[idx].online = 0;
        g_cpus[idx].kernel_stack_top = 0;
        g_cpus[idx].arch = nullptr;
        g_cpus[idx].current = nullptr;
        if (apic_id < 256) g_apicid_to_index[apic_id] = (int)idx;
    }

    //  walk madt processor-local-apic entries (type 0); register each enabled core.
    //  the bsp (its own apic id) is registered first so it always gets index 0. (satoru)
    void enumerate_from_madt(const AcpiSdt* madt, uint32_t bsp_apic) {
        register_cpu(bsp_apic);
        const uint8_t* p = (const uint8_t*)madt + sizeof(AcpiSdt) + 8;  // skip lapic addr + flags (satoru)
        const uint8_t* end = (const uint8_t*)madt + madt->length;
        while (p + 2 <= end) {
            uint8_t type = p[0];
            uint8_t len  = p[1];
            if (len < 2) break;  // malformed  -  stop (satoru)
            if (type == 0 && len >= 8) {         // processor local apic (satoru)
                uint8_t apic_id = p[3];
                uint32_t flags = *(const uint32_t*)(p + 4);
                if (flags & 1)                    // bit0 = enabled (satoru)
                    register_cpu(apic_id);
            }
            p += len;
        }
    }
}

void SMP::Init() {
    if (g_inited) return;
    g_inited = true;

    for (int i = 0; i < 256; i++) g_apicid_to_index[i] = -1;
    g_cpu_count = 0;

    // lapic mmio base from ia32_apic_base (msr 0x1b), masked to the page. (satoru)
    uint64_t apic_base_msr = rdmsr(0x1B);
    g_lapic_base = apic_base_msr & 0xFFFFF000ULL;
    if (g_lapic_base == 0) g_lapic_base = 0xFEE00000ULL;

    LapicEnable();
    uint32_t bsp_apic = ApicId();

    uint64_t rsdp = find_rsdp();
    const AcpiSdt* madt = rsdp ? find_madt(rsdp) : nullptr;
    if (madt) {
        enumerate_from_madt(madt, bsp_apic);
        SerialLogger::Log("[SMP] MADT found: ");
    } else {
        // fallback: trust the cpuid logical-core count, assume sequential apic ids
        // 0..n-1 with the bsp first. (satoru)
        register_cpu(bsp_apic);
        int n = CPUDetect::GetThreadCount();
        if (n < 1) n = 1;
        for (int id = 0; id < n && g_cpu_count < SMP_MAX_CPUS; id++)
            if ((uint32_t)id != bsp_apic) register_cpu((uint32_t)id);
        SerialLogger::Log("[SMP] no MADT, cpuid fallback: ");
    }

    // bsp is online by definition. (satoru)
    g_cpus[0].online = 1;
    // point the bsp's KERNEL_GS_BASE at its PerCpu block so the reworked SYSCALL
    // stub can find this cpu's kernel stack via gs after swapgs. (satoru)
    SetupGsBase();

    SerialLogger::LogDec((int)g_cpu_count);
    SerialLogger::Log(" cpu(s), bsp apic id ");
    SerialLogger::LogDec((int)bsp_apic);
    SerialLogger::Log(", lapic @ ");
    SerialLogger::LogHex((uint32_t)g_lapic_base);
    SerialLogger::Log("\r\n");
    for (uint32_t i = 0; i < g_cpu_count; i++) {
        SerialLogger::Log("[SMP]   cpu");
        SerialLogger::LogDec((int)i);
        SerialLogger::Log(" -> apic id ");
        SerialLogger::LogDec((int)g_cpus[i].apic_id);
        SerialLogger::Log("\r\n");
    }
}

uint32_t SMP::CpuCount() { return g_cpu_count ? g_cpu_count : 1; }

uint32_t SMP::OnlineCount() {
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_cpu_count; i++) if (g_cpus[i].online) n++;
    return n ? n : 1;
}

uint32_t SMP::ApicId() {
    if (g_lapic_base == 0) return 0;
    return LapicRead(0x20) >> 24;   // local apic id register (satoru)
}

uint32_t SMP::CpuIndex() {
    uint32_t id = ApicId();
    if (id < 256 && g_apicid_to_index[id] >= 0) return (uint32_t)g_apicid_to_index[id];
    return 0;  // unknown -> treat as bsp (satoru)
}

PerCpu* SMP::Current()            { return &g_cpus[CpuIndex()]; }
PerCpu* SMP::ByIndex(uint32_t i)  { return (i < g_cpu_count) ? &g_cpus[i] : nullptr; }

uint32_t SMP::LapicRead(uint32_t reg)             { return mmio_r32(g_lapic_base + reg); }
void     SMP::LapicWrite(uint32_t reg, uint32_t v){ mmio_w32(g_lapic_base + reg, v); }

void SMP::SetupGsBase() {
    // KERNEL_GS_BASE holds this cpu's PerCpu pointer while in user mode; the
    // SYSCALL stub does swapgs at entry to bring it into gs, reads gs:0/gs:8, and
    // swapgs back before iretq  -  net-zero per syscall, so it permanently holds the
    // per-cpu pointer. (satoru)
    wrmsr(MSR_KERNEL_GS_BASE, (uint64_t)(uintptr_t)Current());
}

void SMP::LapicEnable() {
    // also flip the global-enable bit in ia32_apic_base via the cached value below;
    // qemu/most firmware leave it set, but be explicit. then set the spurious-int
    // vector register (0xf0): bit 8 = apic software enable, low 8 = vector 0xff. (satoru)
    uint32_t svr = LapicRead(0xF0);
    svr |= (1u << 8);          // software enable (satoru)
    svr = (svr & ~0xFFu) | 0xFF;
    LapicWrite(0xF0, svr);
}

//  64-bit entry reached by every application processor via the trampoline. runs
//  on the kernel's shared address space with this ap's own stack. phase 2 just
//  proves the bring-up: enable the local apic, find our per-cpu block, signal
//  online, then idle. phase 3 replaces the idle spin with a scheduler loop.
//  no shared serial here  -  the bsp logs each core after its online flag flips,
//  so there is no cross-core console race. (satoru)
extern "C" void ap_entry() {
    SMP::LapicEnable();
    HAL::SetupAPCpuState();  // own gdt+tss, shared idt, per-core syscall msrs (satoru)
    SMP::SetupGsBase();      // KERNEL_GS_BASE -> this ap's PerCpu (after the gs reload) (satoru)
    PerCpu* me = SMP::Current();
    if (me) me->online = 1;

    //  phase 4  -  arm a per-ap LAPIC timer (vector 0x40, periodic) so user threads
    //  on this ap are PREEMPTED, not just cooperative. calibrate the reload count
    //  against the tsc (irqs are still off here) for a ~100 hz tick, then enable
    //  interrupts on this ap so the timer (the only irq targeted at an ap) fires.
    //  the timer no-ops unless it interrupts ring-3 user code. (satoru)
    {
        SMP::LapicWrite(0x3E0, 0x3);            // divide config: 0x3 = divide by 16 (satoru)
        SMP::LapicWrite(0x320, (1u << 16));     // LVT timer: masked one-shot for calibration (satoru)
        SMP::LapicWrite(0x380, 0xFFFFFFFFu);    // count down from max (satoru)
        busy_us(10000);                          // 10 ms (satoru)
        uint32_t ticks_10ms = 0xFFFFFFFFu - SMP::LapicRead(0x390);
        SMP::LapicWrite(0x380, 0);              // stop (satoru)
        // ~100 hz. (tried 1 khz to tighten thread handshakes  -  firefox stalled
        // right after its thread-spawn burst: the 10x preempt rate on the aps
        // widened a save/resume race window. back to the proven 10 ms grain.)
        // (satoru)
        uint32_t period = ticks_10ms ? ticks_10ms : 1000000u;   // ticks in 10 ms (satoru)
        SMP::LapicWrite(0x320, 0x40u | (1u << 17));   // LVT timer: vector 0x40, periodic (satoru)
        SMP::LapicWrite(0x380, period);                // arm (satoru)
        __asm__ volatile("sti");                       // let the timer fire on this ap (satoru)
    }

    //  dispatch forever (fresh procs and/or resumable threads, per the gates). (satoru)
    ap_dispatch_reenter();
}

//  the ap dispatch loop, extern so the syscall exit path can iret BACK into it
//  (via SMP::ApIdleFrame) when this ap's last thread exits. two claim sources:
//  1) thread resume (kurono.apthreads): pull a Ready sibling thread of a
//     multi-threaded process and iretq into its saved frame  -  from then on the
//     per-ap LAPIC timer (ApTimerPreempt) multiplexes this cpu across the ready
//     threads, giving the process TRUE parallelism with the bsp.
//  2) phase 3d fresh-proc launch (kurono.apsched): claim an explicitly-pinned
//     never-entered user process and run it via RunProcessWithArgs. (satoru)
extern "C" [[noreturn]] void ap_dispatch_reenter() {
    uint32_t idx = SMP::CpuIndex();
    Scheduler::SetCurrentForThisCpu(nullptr);   // no task on this cpu right now (satoru)
    Userspace::SetActiveForThisCpu(nullptr);    // no active user session either (satoru)
    for (;;) {
        if (g_ap_thread_sched) {
            Process* t = Scheduler::ClaimReadyThreadForCpu(idx);
            if (t) {
                {
                    // claim trace  -  resumes are infrequent (an idle ap picking up a
                    // ready sibling), so this stays low-volume. (satoru)
                    uint64_t lf; g_ap_log_lock.LockIrqSave(&lf);
                    SerialLogger::Log("[apr] cpu"); SerialLogger::LogDec((int)idx);
                    SerialLogger::Log(" resume pid="); SerialLogger::LogDec((int)t->pid);
                    SerialLogger::Log("\r\n");
                    g_ap_log_lock.UnlockIrqRestore(lf);
                }
                //  resume the thread from its saved user frame. LoadUserFrame sets
                //  this cpu's current task, tss.rsp0 + gs:8, cr3, fs base and fpu;
                //  the asm pivot then irets into ring-3. never returns  -  the LAPIC
                //  timer preempt keeps this cpu multiplexing ready threads, and a
                //  thread exit with nothing left claimable irets back here. (satoru)
                InterruptFrame f;
                if (Scheduler::LoadUserFrame(t, &f)) {
                    //  the per-cpu userspace-active gate: without it IsActive() is
                    //  false on this ap and the whole linux syscall layer no-ops
                    //  (no frame save, no linux current, sys_exit does nothing  - 
                    //  the thread's exit loop spins forever). (satoru)
                    Userspace::SetActiveForThisCpu(t);
                    //  sync this cpu's linux current-process to the resumed thread,
                    //  or LinuxSyscall::Current() is null on the ap and syscalls
                    //  (mremap stack-probe) loop on ENOMEM (the [eag] pid=-1 spam).
                    //  (satoru)
                    LinuxSyscall::SyncCurrentToTask(t);
                    //  ap_enter_user_frame irets WITHOUT swapgs, so set the active
                    //  gs to the thread's user gs (and park the per-cpu ptr in
                    //  KERNEL_GS_BASE)  -  the no-swapgs form, same as the isr
                    //  preempt path. (satoru)
                    Scheduler::FixupGsAfterIsrSwitch();
                    ap_enter_user_frame(&f);
                }
                //  load failed (no saved frame?)  -  put it back. (satoru)
                t->state = Process_Ready;
                Scheduler::SetCurrentForThisCpu(nullptr);
            }
        }
        if (g_ap_user_sched) {
            Process* t = Scheduler::ClaimFreshUserForCpu(idx);
            if (t) {
                {
                    uint64_t f; g_ap_log_lock.LockIrqSave(&f);
                    SerialLogger::Log("[SMP] cpu");
                    SerialLogger::LogDec((int)idx);
                    SerialLogger::Log(" running user proc '");
                    SerialLogger::Log(t->name);
                    SerialLogger::Log("'\r\n");
                    g_ap_log_lock.UnlockIrqRestore(f);
                }
                int rc = Userspace::RunProcessWithArgs(t, nullptr, nullptr);
                {
                    uint64_t f; g_ap_log_lock.LockIrqSave(&f);
                    // drain what the user program printed (proves it ran on this ap). (satoru)
                    char obuf[256]; int on;
                    while ((on = LinuxSyscall::ReadConsoleOutput(obuf, (int)sizeof(obuf) - 1)) > 0) {
                        obuf[on] = 0;
                        SerialLogger::Log("[SMP] cpu"); SerialLogger::LogDec((int)idx);
                        SerialLogger::Log(" out: "); SerialLogger::Log(obuf); SerialLogger::Log("\r\n");
                    }
                    SerialLogger::Log("[SMP] cpu");
                    SerialLogger::LogDec((int)idx);
                    SerialLogger::Log(" user proc finished, exit=");
                    SerialLogger::LogDec(rc);
                    SerialLogger::Log("\r\n");
                    g_ap_log_lock.UnlockIrqRestore(f);
                }
                continue;
            }
            //  gate on but nothing to claim: spin-poll (no per-ap timer yet). (satoru)
            __asm__ volatile("pause");
            continue;
        }
        if (g_ap_thread_sched) {
            //  thread gate on, nothing claimable right now: back off ~50us so the
            //  idle aps don't hammer g_sched_lock at core speed while the bsp is
            //  trying to schedule. (satoru)
            busy_us(50);
            continue;
        }
        //  gates off: parked exactly as before. the gates are decided at boot from
        //  kurono.apsched / kurono.apthreads (set before the APs start), so a parked
        //  ap never needs to be woken mid-run; an IPI-wake for a runtime toggle is
        //  phase 4. (satoru)
        __asm__ volatile("hlt; pause");
    }
}

void SMP::SetApUserSched(bool on) { g_ap_user_sched = on; }
bool SMP::ApUserSched()           { return g_ap_user_sched; }

void SMP::SetApThreadSched(bool on) { g_ap_thread_sched = on; }
bool SMP::ApThreadSched()           { return g_ap_thread_sched; }

//  ring-0 reentry frame into the dispatch loop, for the syscall exit path: the
//  ap's current thread just exited and nothing else is claimable, so the
//  handler's iretq must land somewhere  -  back at the top of the claim loop, on
//  this cpu's (currently dead) bring-up stack. IF=1 in rflags so the lapic
//  timer + tlb ipis keep firing while the loop spins. (satoru)
void SMP::ApIdleFrame(InterruptFrame* f) {
    if (!f) return;
    for (unsigned long i = 0; i < sizeof(InterruptFrame); i++) ((uint8_t*)f)[i] = 0;
    PerCpu* me = Current();
    f->rip    = (uint64_t)(uintptr_t)&ap_dispatch_reenter;
    f->cs     = (uint64_t)GDT_KERNEL_CODE_SELECTOR;
    f->ss     = (uint64_t)GDT_KERNEL_DATA_SELECTOR;
    f->rsp    = me ? me->kernel_stack_top : 0;
    f->rflags = 0x202ULL;
}

//  tlb shootdown. sender: pulse vector 0x41 at every other cpu (all-excluding-
//  self shorthand) and wait  -  bounded  -  for each online peer to ack. bounded
//  because a peer may briefly sit with IF=0 (a spinlock hold); the per-ap timer
//  cr3 reload in ApTimerPreempt bounds any missed window to one tick. (satoru)
void SMP::BroadcastTlbFlush() {
    uint32_t online = OnlineCount();
    if (online <= 1) return;
    __atomic_store_n(&g_tlb_ack, 0u, __ATOMIC_RELEASE);
    //  wait for any prior ipi to leave the icr (delivery status, bit 12). (satoru)
    for (int i = 0; i < 100000 && (LapicRead(0x300) & (1u << 12)); i++)
        __asm__ volatile("pause");
    //  fixed delivery, assert, destination shorthand 11 = all excluding self. (satoru)
    LapicWrite(0x300, (3u << 18) | (1u << 14) | 0x41u);
    uint32_t want = online - 1;
    for (int i = 0; i < 400000; i++) {   // ~sub-ms bound (satoru)
        if (__atomic_load_n(&g_tlb_ack, __ATOMIC_ACQUIRE) >= want) return;
        __asm__ volatile("pause");
    }
    //  timed out  -  a peer had interrupts masked; its next timer tick reloads
    //  cr3, so proceed rather than wedge the unmap path. (satoru)
}

void SMP::HandleTlbIpi() {
    uint64_t c3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(c3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(c3) : "memory");
    __atomic_fetch_add(&g_tlb_ack, 1u, __ATOMIC_ACQ_REL);
}

void SMP::StartAPs() {
    if (g_cpu_count <= 1) {
        SerialLogger::Log("[SMP] single cpu, no APs to start\r\n");
        return;
    }

    // copy the trampoline blob to physical 0x8000 (identity-mapped, pmm-reserved
    // low memory). the SIPI start vector is 0x8000 >> 12 = 0x08. (satoru)
    uint8_t* tramp = (uint8_t*)(uintptr_t)0x8000;
    uint64_t sz = (uint64_t)(_binary_ap_trampoline_bin_end -
                             _binary_ap_trampoline_bin_start);
    for (uint64_t i = 0; i < sz; i++) tramp[i] = _binary_ap_trampoline_bin_start[i];

    // patch area at 0x9000  -  fields the trampoline reads on its way up. (satoru)
    volatile uint64_t* p_cr3   = (volatile uint64_t*)(uintptr_t)0x9000;
    volatile uint8_t*  p_gdt   = (volatile uint8_t*) (uintptr_t)0x9008;
    volatile uint64_t* p_entry = (volatile uint64_t*)(uintptr_t)0x9018;
    volatile uint64_t* p_stack = (volatile uint64_t*)(uintptr_t)0x9020;

    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    *p_cr3 = cr3;

    struct __attribute__((packed)) { uint16_t limit; uint64_t base; } gdtr;
    __asm__ volatile("sgdt %0" : "=m"(gdtr));
    const uint8_t* g = (const uint8_t*)&gdtr;
    for (int k = 0; k < 10; k++) p_gdt[k] = g[k];

    *p_entry = (uint64_t)(uintptr_t)&ap_entry;

    const uint8_t vector = 0x08;   // 0x8000 >> 12 (satoru)
    const uint32_t STACK_SZ = 16 * 1024;

    for (uint32_t i = 1; i < g_cpu_count; i++) {
        void* stk = PMM::AllocBytes(STACK_SZ);
        if (!stk) {
            SerialLogger::Log("[SMP] cpu"); SerialLogger::LogDec((int)i);
            SerialLogger::Log(" no stack, skipped\r\n");
            continue;
        }
        *p_stack = (uint64_t)(uintptr_t)stk + STACK_SZ;   // top, grows down (satoru)
        g_cpus[i].kernel_stack_top = *p_stack;
        g_cpus[i].online = 0;

        uint32_t apic = g_cpus[i].apic_id;

        // INIT-SIPI-SIPI (intel mp init protocol). assert INIT, wait 10ms, then
        // two STARTUP IPIs ~200us apart carrying the trampoline vector. (satoru)
        LapicWrite(0x310, apic << 24);
        LapicWrite(0x300, 0x4500);          // INIT, assert, edge (satoru)
        busy_us(10000);
        for (int s = 0; s < 2; s++) {
            LapicWrite(0x310, apic << 24);
            LapicWrite(0x300, 0x4600 | vector);  // STARTUP + trampoline vector (satoru)
            busy_us(200);
        }

        // wait for the ap to flip its online flag (~100ms budget). (satoru)
        int tries = 100;
        while (!g_cpus[i].online && tries-- > 0) busy_us(1000);

        SerialLogger::Log("[SMP] cpu"); SerialLogger::LogDec((int)i);
        SerialLogger::Log(g_cpus[i].online ? " online (apic " : " FAILED to start (apic ");
        SerialLogger::LogDec((int)apic);
        SerialLogger::Log(")\r\n");
    }

    SerialLogger::Log("[SMP] "); SerialLogger::LogDec((int)OnlineCount());
    SerialLogger::Log("/"); SerialLogger::LogDec((int)g_cpu_count);
    SerialLogger::Log(" cpus online\r\n");
}
// end (satoru)
