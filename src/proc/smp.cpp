#include "smp.h"
#include "../kernel/dbgwatch.h"   // (satoru)
#include "../kernel/kimage.h"   // the trampoline lgdt is 32-bit: gdt base must be physical (satoru)
#include "../kernel/vmm.h"       // cr3 must fit in 32 bits for the AP trampoline (satoru)
#include "../ui/boot_anim.h"
#include "../drivers/serial.h"
#include "../drivers/cpu_detect.h"
#include "../kernel/pmm.h"
#include "../hal/hal.h"
#include "scheduler.h"            // ap dispatch: claim + run user procs (smp 3d) (satoru)
#include "spinlock.h"
#include "../kernel/userspace.h"  // Userspace::RunProcessWithArgs on the ap (satoru)
#include "../linux/linux_syscall.h" // drain the ap process's console output (satoru)
#include "../drivers/timer.h"     // wall-clock bound for the tlb-shootdown ack wait (satoru)

//  the ap trampoline blob (flat binary, embedded via objcopy) - copied to phys
//  0x8000 before the first SIPI. (satoru)
extern "C" uint8_t _binary_ap_trampoline_bin_start[];
extern "C" uint8_t _binary_ap_trampoline_bin_end[];

//  switch_to.asm: pivot onto a saved InterruptFrame and iretq into ring-3 - 
//  how an ap RESUMES a claimed sibling thread. never returns. (satoru)
extern "C" [[noreturn]] void ap_enter_user_frame(InterruptFrame* f);

//  the ap dispatch loop (defined below ap_entry) - also the iret target the
//  syscall exit path uses when an ap runs out of threads. (satoru)
extern "C" [[noreturn]] void ap_dispatch_reenter();

//  smp phase 1 - lapic enable + cpu enumeration + per-cpu blocks. see smp.h. (satoru)

namespace {
    //  per-cpu blocks + the apic-id -> dense-index map (apic ids can be sparse). (satoru)
    PerCpu   g_cpus[SMP_MAX_CPUS];
    int      g_apicid_to_index[256];
    uint32_t g_cpu_count = 0;
    uint64_t g_ap_stack_bytes = 0;   // stack frames handed to the aps (census) (satoru)
    uint64_t g_lapic_base = 0;     // identity-mapped xapic mmio window (satoru)
    bool     g_inited = false;

    //  phase 3d gate + a lock so two cores' ap-dispatch serial lines don't interleave. (satoru)
    volatile bool g_ap_user_sched = false;
    Spinlock      g_ap_log_lock;

    //  thread-dispatch gate (kurono.apthreads): aps also resume ready sibling
    //  threads, giving a multi-threaded process true parallelism. (satoru)
    volatile bool g_ap_thread_sched = false;

    //  sched=2: per-cpu "halted in the dispatch loop" flags, set under cli right
    //  before sti;hlt and claimed (cas 1->0) by the waker that sends the ipi. (satoru)
    volatile uint8_t g_cpu_idle[SMP_MAX_CPUS] = {};
    // each ap's calibrated lapic count for 10 ms (ap_entry), so a service cpu can
    // retune its tick later (SetLapicTickHz). (satoru)
    uint32_t g_lapic_10ms[SMP_MAX_CPUS] = {};
    //  sched=2: CpuIndex via IA32_TSC_AUX (rdtscp). (satoru)
    volatile bool g_tscaux = false;
    //  sched=2 tlb acks: each cpu records the broadcast round it flushed for
    //  (read BEFORE its cr3 reload, so a recorded round always means a flush that
    //  came after that round's pte changes). a late ack from an old round can no
    //  longer be counted toward a new one, which the shared counter allowed once
    //  halted cpus ack late. (satoru)
    volatile uint64_t g_tlb_acked_seq[SMP_MAX_CPUS] = {};
    const uint32_t MSR_TSC_AUX = 0xC0000103u;

    //  tlb-shootdown ack counter: receivers increment after reloading cr3. (satoru)
    volatile uint32_t g_tlb_ack = 0;

    //  the kernel address-space root, captured in StartAPs, for LoadKernelCr3.
    //  an idle/parked cpu switches here so it never walks a dead process's
    //  (possibly destroyed + recycled) page tables. (satoru)
    volatile uint64_t g_kernel_cr3 = 0;

    //  flush epochs (see smp.h): start-seq bumps at broadcast entry, full-seq
    //  latches the highest start-seq that got acks from every online peer.
    //  senders are serialized by the callers' mmap/kls locks; receivers only
    //  touch g_tlb_ack. (satoru)
    volatile uint64_t g_tlb_flush_start_seq = 0;
    volatile uint64_t g_tlb_flush_full_seq  = 0;
    //  count of shootdowns that gave up unacked - a process-context reader can
    //  print this; the hot path must not do serial i/o (it may be IF=0). (satoru)
    volatile uint32_t g_tlb_flush_timeouts  = 0;

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

    //  x2apic handling, modelled on linux arch/x86/kernel/apic/apic.c. firmware that
    //  hands over with IA32_APIC_BASE.EXTD (bit 10) set kills the xapic mmio window, so
    //  every LapicRead/Write (ids, eoi, ipis, timer) hit dead memory. g_x2apic means
    //  "locked in x2apic mode": the lapic is then driven through the 0x800 msr block. (satoru)
    bool     g_x2apic = false;
    int      g_x2apic_test = 0;         // kurono.x2apictest=1: force x2apic on first; =2: also pretend it is locked (satoru)
    uint32_t g_icr_hi = 0;              // xapic-style icr high half, folded into msr 0x830 (satoru)
    bool     g_lapic_dead = false;      // apic left globally disabled and could not be fixed (satoru)

    inline void cpuid_ex(uint32_t leaf, uint32_t sub, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
        __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
    }

    //  linux x2apic_hw_locked: ARCH_CAPABILITIES (msr 0x10a, cpuid.7.0:edx[29]) bit 21 says
    //  IA32_XAPIC_DISABLE_STATUS (msr 0xbd) exists; its bit 0 = legacy xapic permanently off. (satoru)
    bool x2apic_hw_locked() {
        uint32_t a, b, c, d;
        cpuid_ex(0, 0, &a, &b, &c, &d);
        if (a < 7) return false;
        cpuid_ex(7, 0, &a, &b, &c, &d);
        if (!(d & (1u << 29))) return false;
        if (!(rdmsr(0x10A) & (1ULL << 21))) return false;
        return (rdmsr(0xBD) & 1ULL) != 0;
    }

    //  linux __x2apic_disable: both enable bits off, then xapic only. result: 0 = was not
    //  in x2apic mode, 1 = switched back to xapic and verified, 2 = locked (stays x2apic),
    //  3 = write did not take (apic state unknown). (satoru)
    int x2apic_to_xapic() {
        uint64_t m = rdmsr(0x1B);
        if (!(m & (1ULL << 10))) return 0;
        if (x2apic_hw_locked()) return 2;
        wrmsr(0x1B, m & ~((1ULL << 10) | (1ULL << 11)));
        wrmsr(0x1B, m & ~(1ULL << 10));
        uint64_t r = rdmsr(0x1B);
        if (r & (1ULL << 10)) return 3;
        if (!(r & (1ULL << 11))) return 3;
        return 1;
    }

    //  busy-wait - interrupts are disabled at this point in boot so we cannot
    //  sleep; spin on the (already-calibrated) tsc instead. (satoru)
    void busy_us(uint64_t us) {
        uint64_t hz = (uint64_t)CPUDetect::GetInfo().frequency.tsc_frequency;
        if (hz < 1000000ULL) hz = 2000000000ULL;   // ~2ghz fallback (satoru)
        uint64_t ticks = (hz / 1000000ULL) * us;
        uint64_t start = rdtsc();
        // iteration cap too: a stuck/miscalibrated tsc must not hang boot (satoru)
        uint64_t cap = us * 4000ULL + 1000ULL;
        while (rdtsc() - start < ticks && cap--) __asm__ volatile("pause");
    }

    //  firmware IA32_TSC_ADJUST (msr 0x3b) sanitize, modelled on linux
    //  arch/x86/kernel/tsc_sync.c (tsc_sanitize_first_cpu / tsc_store_and_check_tsc_adjust).
    //  rdtsc returns the core's counter plus this msr. the owner's arrow lake laptop
    //  hands over with a different value per cpu (linux logs "[Firmware Bug]: TSC
    //  ADJUST: CPU0: -1797404919 force to 0" and "differs within socket(s)" on every
    //  boot), so the bsp's rdtsc was 0.6 to 0.9 s behind the aps'. every tsc clock in
    //  the kernel (Timer::GetRealMs64, the display drivers' timeouts) then jumped by
    //  that much whenever a timestamp taken on one cpu was compared on another: the
    //  page flip driver saw "250 ms without a latch" a few ms after the boot
    //  animation's ap handed the screen to the gui on the bsp. the counters under the
    //  msr are synchronous from reset, so writing 0 on every cpu makes rdtsc agree
    //  everywhere. it runs before anything keeps a tsc base (bsp: early in
    //  kernel_main, aps: first thing in ap_entry). kurono.tscadjust=0 keeps the
    //  firmware values. (satoru)
    const uint32_t MSR_IA32_TSC_ADJUST = 0x3Bu;
    bool    g_tscadj_fix = true;
    bool    g_tscadj_have = false;                 // cpuid.7.0:ebx[1] on the bsp (satoru)
    int64_t g_tscadj_boot[SMP_MAX_CPUS] = {};      // firmware value per dense cpu index (satoru)
    uint32_t g_tscadj_nonzero = 0;

    //  cr0/cr4 policy (kurono.wp, see SMP::CrPolicyBsp). the values each cpu came
    //  in with and left with, so the bsp can log the aps (no serial on an ap). (satoru)
    const uint64_t CR0_WP_BIT = 1ULL << 16;
    const uint64_t CR0_NE_BIT = 1ULL << 5;
    const uint64_t CR4_POLICY_MASK = (1ULL << 20) | (1ULL << 11);   // smep, umip (satoru)
    volatile bool g_wp_on = false;
    uint64_t g_cr4_bsp_set = 0;
    uint64_t g_cr0_in[SMP_MAX_CPUS] = {}, g_cr0_out[SMP_MAX_CPUS] = {};
    uint64_t g_cr4_in[SMP_MAX_CPUS] = {}, g_cr4_out[SMP_MAX_CPUS] = {};
    volatile uint8_t g_wp_suspended[SMP_MAX_CPUS] = {};
    volatile uint32_t g_wp_suspended_any = 0;   // fast path for WpRearm (satoru)
    uint64_t g_wp_fault_rip[SMP_MAX_CPUS] = {};  // the store WP was suspended for (satoru)
    uint64_t g_wp_last_va[SMP_MAX_CPUS] = {};
    volatile uint32_t g_wp_fallback_logs = 0;
    volatile uint64_t g_wp_fallback_count = 0;
    uint64_t g_wp_stale_va[SMP_MAX_CPUS] = {};
    uint32_t g_wp_stale_n[SMP_MAX_CPUS] = {};

    inline uint64_t read_cr0() { uint64_t v; __asm__ volatile("mov %%cr0, %0" : "=r"(v)); return v; }
    inline uint64_t read_cr4() { uint64_t v; __asm__ volatile("mov %%cr4, %0" : "=r"(v)); return v; }
    inline void write_cr0(uint64_t v) { __asm__ volatile("mov %0, %%cr0" :: "r"(v) : "memory"); }
    inline void write_cr4(uint64_t v) { __asm__ volatile("mov %0, %%cr4" :: "r"(v) : "memory"); }

    void cr_log(uint32_t idx) {
        SerialLogger::Log("[cr] cpu"); SerialLogger::LogDec((int)idx);
        SerialLogger::Log(" cr0 in="); SerialLogger::LogHex64(g_cr0_in[idx]);
        SerialLogger::Log(" out="); SerialLogger::LogHex64(g_cr0_out[idx]);
        SerialLogger::Log(" wp "); SerialLogger::LogDec((g_cr0_in[idx] & CR0_WP_BIT) ? 1 : 0);
        SerialLogger::Log("->"); SerialLogger::LogDec((g_cr0_out[idx] & CR0_WP_BIT) ? 1 : 0);
        SerialLogger::Log(" cd "); SerialLogger::LogDec((g_cr0_out[idx] >> 30) & 1 ? 1 : 0);
        SerialLogger::Log(" | cr4 in="); SerialLogger::LogHex64(g_cr4_in[idx]);
        SerialLogger::Log(" out="); SerialLogger::LogHex64(g_cr4_out[idx]);
        SerialLogger::Log(" smep "); SerialLogger::LogDec((g_cr4_out[idx] >> 20) & 1 ? 1 : 0);
        SerialLogger::Log(" smap "); SerialLogger::LogDec((g_cr4_out[idx] >> 21) & 1 ? 1 : 0);
        SerialLogger::Log(" umip "); SerialLogger::LogDec((g_cr4_out[idx] >> 11) & 1 ? 1 : 0);
        SerialLogger::Log("\r\n");
    }

    bool tsc_adjust_supported() {
        uint32_t a, b, c, d;
        cpuid_ex(0, 0, &a, &b, &c, &d);
        if (a < 7) return false;
        cpuid_ex(7, 0, &a, &b, &c, &d);
        return (b & (1u << 1)) != 0;
    }

    //  returns what the firmware left on the calling cpu; writes 0 over a nonzero
    //  value unless the kill switch is set. no serial (an ap calls it). (satoru)
    int64_t tsc_adjust_sanitize_this_cpu() {
        if (!g_tscadj_have || !tsc_adjust_supported()) return 0;
        int64_t v = (int64_t)rdmsr(MSR_IA32_TSC_ADJUST);
        if (v != 0 && g_tscadj_fix) wrmsr(MSR_IA32_TSC_ADJUST, 0);
        return v;
    }

    void tsc_adjust_log(uint32_t idx, int64_t v) {
        SerialLogger::Log("[tsc] cpu"); SerialLogger::LogDec((int)idx);
        SerialLogger::Log(" firmware tsc_adjust 0x"); SerialLogger::LogHex64((uint64_t)v);
        // ticks do not fit LogDec; the ms figure is what matters (2.88 ghz laptop
        // tsc: 2880000 ticks per ms, the calibrated rate is not known this early
        // on the bsp, so the line gives mega-ticks). (satoru)
        int64_t mt = v / 1000000;
        SerialLogger::Log(" ("); SerialLogger::LogDec((int)mt); SerialLogger::Log(" M ticks)");
        SerialLogger::Log(g_tscadj_fix ? ", forced to 0\r\n" : ", KEPT (kurono.tscadjust=0)\r\n");
    }

    //  wait (bounded) for the previous ipi to leave the icr (delivery status, bit 12). (satoru)
    void icr_idle() {
        for (int i = 0; i < 100000 && (SMP::LapicRead(0x300) & (1u << 12)); i++)
            __asm__ volatile("pause");
    }

    inline uint32_t mmio_r32(uint64_t addr) {
        return *(volatile uint32_t*)(uintptr_t)addr;
    }
    inline void mmio_w32(uint64_t addr, uint32_t val) {
        *(volatile uint32_t*)(uintptr_t)addr = val;
    }
    //  one ipi, atomically: x2apic takes the full 32-bit destination and the low
    //  half in ONE write to msr 0x830 (the old split 0x310/0x300 emulation kept
    //  the high half in a global, so two cpus sending at once could swap
    //  destinations, and it kept only 8 id bits). wrmsr to the x2apic block is not
    //  serializing: fence first like linux weak_wrmsr_fence, so the stores the
    //  target will look at are visible before the ipi lands. xapic: the two mmio
    //  writes as before. (satoru)
    void icr_send(uint32_t dest, uint32_t low) {
        if (g_lapic_dead) return;
        if (g_x2apic) {
            __asm__ volatile("mfence; lfence" ::: "memory");
            wrmsr(0x830, ((uint64_t)dest << 32) | low);
            return;
        }
        mmio_w32(g_lapic_base + 0x310, dest << 24);
        mmio_w32(g_lapic_base + 0x300, low);
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

    //  boot-provided rsdp copy (see BootAcpi in smp.h) + the maxcpus request. (satoru)
    uint8_t  g_boot_rsdp[64];
    uint32_t g_boot_rsdp_len = 0;
    bool     g_boot_efi = false;
    uint64_t g_boot_systab = 0;
    uint32_t g_max_cpus_req = 0;

    //  the kernel identity map covers 0..512 GiB (kurono_boot.asm, 2 MiB pages), so
    //  acpi memory is read in place; refuse anything outside it (never fault). (satoru)
    const uint64_t ACPI_MAP_LIMIT = 512ULL << 30;

    //  a sane, mapped sdt: header inside the identity map, length plausible. (satoru)
    const AcpiSdt* sdt_at(uint64_t phys) {
        if (!phys || phys + sizeof(AcpiSdt) > ACPI_MAP_LIMIT) return nullptr;
        const AcpiSdt* t = (const AcpiSdt*)(uintptr_t)phys;
        if (t->length < sizeof(AcpiSdt) || t->length > (16u << 20)) return nullptr;
        if (phys + t->length > ACPI_MAP_LIMIT) return nullptr;
        return t;
    }

    //  locate the madt ("APIC") via the rsdt (acpi 1.0) or xsdt (acpi 2.0+), logging
    //  every table signature found (handy for later i2c/hid work). (satoru)
    const AcpiSdt* find_madt(uint64_t rsdp_phys) {
        const uint8_t* rsdp = (const uint8_t*)(uintptr_t)rsdp_phys;
        uint8_t revision = rsdp[15];
        uint64_t xsdt_phys = (revision >= 2) ? *(const uint64_t*)(rsdp + 24) : 0;
        uint32_t rsdt_phys = *(const uint32_t*)(rsdp + 16);
        const AcpiSdt* madt = nullptr;
        const AcpiSdt* root = xsdt_phys ? sdt_at(xsdt_phys) : nullptr;
        bool wide = root != nullptr;
        if (!root) root = sdt_at(rsdt_phys);   // no/invalid xsdt: fall back to rsdt (satoru)
        SerialLogger::Log("[ACPI] rev "); SerialLogger::LogDec(revision);
        SerialLogger::Log(" xsdt "); SerialLogger::LogHex64(xsdt_phys);
        SerialLogger::Log(" rsdt "); SerialLogger::LogHex64(rsdt_phys);
        SerialLogger::Log(wide ? " (using xsdt)\r\n" : (root ? " (using rsdt)\r\n" : " (no usable root table)\r\n"));
        if (!root) return nullptr;
        uint32_t esz = wide ? 8 : 4;
        uint32_t n = (root->length - sizeof(AcpiSdt)) / esz;
        const uint8_t* ptrs = (const uint8_t*)root + sizeof(AcpiSdt);
        SerialLogger::Log("[ACPI] tables ("); SerialLogger::LogDec((int)n); SerialLogger::Log("):");
        for (uint32_t i = 0; i < n; i++) {
            uint64_t tp = wide ? *(const uint64_t*)(ptrs + i * 8) : *(const uint32_t*)(ptrs + i * 4);
            const AcpiSdt* t = sdt_at(tp);
            if (!t) { SerialLogger::Log(" ????"); continue; }
            char sg[6] = { (char)t->signature[0], (char)t->signature[1],
                           (char)t->signature[2], (char)t->signature[3], 0, 0 };
            SerialLogger::Log(" "); SerialLogger::Log(sg);
            if (!madt && sig4(t->signature, "APIC")) {
                if (checksum((const uint8_t*)t, t->length) != 0)
                    SerialLogger::Log("(bad-csum)");   // linux warns and uses it too (satoru)
                madt = t;
            }
        }
        SerialLogger::Log("\r\n");
        return madt;
    }

    //  candidate apic ids from the madt, in table order (bsp first), deduped. (satoru)
    uint32_t g_cand[256];
    uint32_t g_cand_n = 0;
    uint32_t g_cand_skipped = 0;     // unreachable (>=255) or not-enabled entries (satoru)

    void add_candidate(uint32_t id) {
        for (uint32_t i = 0; i < g_cand_n; i++) if (g_cand[i] == id) return;
        if (g_cand_n < 256) g_cand[g_cand_n++] = id;
    }

    //  apic id -> dense index. x2apic ids can be >= 256 (the 256-entry map only
    //  covers xapic ids), so larger ones fall back to a scan of the registered
    //  cpus. -1 = unknown. (satoru)
    int apicid_index(uint32_t apic_id) {
        if (apic_id < 256) return g_apicid_to_index[apic_id];
        for (uint32_t i = 0; i < g_cpu_count; i++) if (g_cpus[i].apic_id == apic_id) return (int)i;
        return -1;
    }

    void register_cpu(uint32_t apic_id) {
        if (g_cpu_count >= SMP_MAX_CPUS) return;
        if (apicid_index(apic_id) >= 0) return;  // dup (satoru)
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

    //  walk madt entries: type 0 (processor local apic) and type 9 (processor local
    //  x2apic), as linux acpi_parse_lapic / acpi_parse_x2apic do. flags bit0 =
    //  enabled, bit1 = online-capable. like linux, a core that is only
    //  online-capable (not enabled) is a hotplug slot: counted, never started at
    //  boot. the xapic mmio ipi path reaches ids 0..254 only, so larger x2apic ids
    //  are skipped (logged), never sent a bad ipi. results land in g_cand[]. (satoru)
    void parse_madt(const AcpiSdt* madt, uint32_t bsp_apic) {
        add_candidate(bsp_apic);
        uint32_t n0 = 0, n9 = 0, hot = 0, big = 0;
        const uint8_t* p = (const uint8_t*)madt + sizeof(AcpiSdt) + 8;  // skip lapic addr + flags (satoru)
        const uint8_t* end = (const uint8_t*)madt + madt->length;
        while (p + 2 <= end) {
            uint8_t type = p[0];
            uint8_t len  = p[1];
            if (len < 2 || p + len > end) break;  // malformed - stop (satoru)
            uint32_t id = 0xFFFFFFFFu, flags = 0;
            bool have = false;
            if (type == 0 && len >= 8) {            // processor local apic (satoru)
                id = p[3]; flags = *(const uint32_t*)(p + 4);
                if (id == 0xFF) id = 0xFFFFFFFFu;   // 0xff = broadcast, not a cpu (satoru)
                have = true; n0++;
            } else if (type == 9 && len >= 16) {    // processor local x2apic (satoru)
                id = *(const uint32_t*)(p + 4); flags = *(const uint32_t*)(p + 8);
                have = true; n9++;
            }
            if (have && id != 0xFFFFFFFFu) {
                if (flags & 1) {
                    // the x2apic icr carries a 32-bit destination, so with the lapic in
                    // x2apic mode every enabled id is reachable. (satoru)
                    if (id >= 255 && !g_x2apic) { big++; g_cand_skipped++; }   // xapic cannot address it (satoru)
                    else add_candidate(id);
                } else if (flags & 2) { hot++; g_cand_skipped++; }
            }
            p += len;
        }
        SerialLogger::Log("[SMP] madt entries: type0="); SerialLogger::LogDec((int)n0);
        SerialLogger::Log(" type9="); SerialLogger::LogDec((int)n9);
        SerialLogger::Log(" usable="); SerialLogger::LogDec((int)g_cand_n);
        SerialLogger::Log(" hotplug-only="); SerialLogger::LogDec((int)hot);
        SerialLogger::Log(" id>=255-skipped="); SerialLogger::LogDec((int)big);
        SerialLogger::Log("\r\n");
    }
}

void BootAcpi::SetRsdp(const uint8_t* src, uint32_t avail) {
    if (!src || avail < 20 || !sig8(src, "RSD PTR ") || checksum(src, 20) != 0) return;
    uint32_t len = 20;
    uint8_t rev = src[15];
    bool v2 = false;
    if (rev >= 2 && avail >= 36) {
        uint32_t l = *(const uint32_t*)(src + 20);
        if (l >= 36 && l <= sizeof(g_boot_rsdp) && l <= avail && checksum(src, l) == 0) { len = l; v2 = true; }
    }
    for (uint32_t i = 0; i < sizeof(g_boot_rsdp); i++) g_boot_rsdp[i] = i < len ? src[i] : 0;
    if (!v2 && rev >= 2) {              // bad extended part: present it as a v1 rsdp (satoru)
        g_boot_rsdp[8]  = (uint8_t)(g_boot_rsdp[8] + rev);
        g_boot_rsdp[15] = 0;
    }
    g_boot_rsdp_len = len;
}
void     BootAcpi::SetEfiBoot() { g_boot_efi = true; }
void     BootAcpi::SetSystemTable(uint64_t st) { g_boot_systab = st; }
uint64_t BootAcpi::SystemTable() { return g_boot_systab; }
uint64_t BootAcpi::Rsdp()       { return g_boot_rsdp_len ? (uint64_t)(uintptr_t)g_boot_rsdp : 0; }
bool     BootAcpi::IsEfiBoot()  { return g_boot_efi; }
void     SMP::SetMaxCpus(uint32_t n) { g_max_cpus_req = n; }
void     SMP::SetX2apicTest(int mode) { g_x2apic_test = mode; }

void SMP::SelfTestMadt() {
    alignas(8) static uint8_t t[128];
    for (unsigned i = 0; i < sizeof(t); i++) t[i] = 0;
    uint8_t* p = t + sizeof(AcpiSdt) + 8;
    auto lapic = [&](uint8_t id, uint32_t fl) { p[0] = 0; p[1] = 8; p[2] = id; p[3] = id; *(uint32_t*)(p + 4) = fl; p += 8; };
    auto x2 = [&](uint32_t id, uint32_t fl) { p[0] = 9; p[1] = 16; *(uint32_t*)(p + 4) = id; *(uint32_t*)(p + 8) = fl; p += 16; };
    lapic(0, 1);        // bsp (satoru)
    x2(300, 1);         // unreachable by xapic: skipped (satoru)
    x2(20, 1);          // usable (satoru)
    x2(21, 2);          // online-capable only: hotplug, skipped (satoru)
    lapic(0, 1);        // duplicate (satoru)
    x2(5, 1);           // usable (satoru)
    lapic(9, 0);        // disabled: skipped silently (satoru)
    ((AcpiSdt*)t)->length = (uint32_t)(p - t);
    uint32_t sn = g_cand_n, ss = g_cand_skipped;
    g_cand_n = 0; g_cand_skipped = 0;
    parse_madt((const AcpiSdt*)t, 0);
    bool ok = g_cand_n == 3 && g_cand[0] == 0 && g_cand[1] == 20 && g_cand[2] == 5 && g_cand_skipped == 2;
    SerialLogger::Log(ok ? "[SMP] madt selftest PASS (expect usable=3 hotplug=1 big=1)\r\n"
                         : "[SMP] madt selftest FAIL\r\n");
    g_cand_n = sn; g_cand_skipped = ss;
}

void SMP::Init() {
    if (g_inited) return;
    g_inited = true;

    for (int i = 0; i < 256; i++) g_apicid_to_index[i] = -1;
    g_cpu_count = 0;

    // lapic mmio base from ia32_apic_base (msr 0x1b), masked to the page. (satoru)
    uint64_t apic_base_msr = rdmsr(0x1B);

    // kurono.x2apictest=1: put the bsp into x2apic mode first, so the disable path below can
    // be exercised without firmware that does it. xapic -> x2apic is the legal transition. (satoru)
    if (g_x2apic_test) {
        uint32_t a, b, c, d;
        cpuid_ex(1, 0, &a, &b, &c, &d);
        if ((c & (1u << 21)) && (apic_base_msr & (1ULL << 11)) && !(apic_base_msr & (1ULL << 10))) {
            wrmsr(0x1B, apic_base_msr | (1ULL << 10));
            apic_base_msr = rdmsr(0x1B);
            SerialLogger::Log("[SMP] x2apictest: forced x2apic on, apic_base now ");
            SerialLogger::LogHex64(apic_base_msr); SerialLogger::Log("\r\n");
        } else {
            SerialLogger::Log("[SMP] x2apictest: cpu cannot enter x2apic here, nothing forced\r\n");
        }
    }

    // firmware-enabled x2apic: switch back to xapic BEFORE any lapic mmio access (as linux
    // __x2apic_disable). if the hardware locks x2apic on, drive the lapic via msrs instead. (satoru)
    if (apic_base_msr & (1ULL << 10)) {
        SerialLogger::Log("[SMP] x2apic enabled by firmware, apic_base "); SerialLogger::LogHex64(apic_base_msr);
        SerialLogger::Log("\r\n");
        int xr = (g_x2apic_test == 2) ? 2 : x2apic_to_xapic();   // =2 exercises the locked (msr driven) path (satoru)
        apic_base_msr = rdmsr(0x1B);
        if (xr == 1) {
            SerialLogger::Log("[SMP] x2apic disabled, back in xapic mode, apic_base now ");
            SerialLogger::LogHex64(apic_base_msr); SerialLogger::Log("\r\n");
        } else if (xr == 2) {
            g_x2apic = true;
            SerialLogger::Log(g_x2apic_test == 2
                ? "[SMP] x2apic treated as LOCKED on (kurono.forcex2apic / x2apictest=2): driving the lapic through msrs, aps started via the x2apic icr\r\n"
                : "[SMP] x2apic is LOCKED on (legacy xapic disabled by hardware): driving the lapic through msrs, aps started via the x2apic icr\r\n");
        } else {
            g_lapic_dead = true;
            SerialLogger::Log("[SMP] !!! x2apic disable did not take, apic_base "); SerialLogger::LogHex64(apic_base_msr);
            SerialLogger::Log(": lapic unusable, single cpu only\r\n");
        }
    }
    g_lapic_base = apic_base_msr & 0xFFFFF000ULL;
    if (g_lapic_base == 0) g_lapic_base = 0xFEE00000ULL;

    if (!g_lapic_dead) LapicEnable();
    uint32_t bsp_apic = g_lapic_dead ? 0 : ApicId();

    SerialLogger::Log("[SMP] apic_base msr "); SerialLogger::LogHex64(apic_base_msr);
    SerialLogger::Log(g_x2apic ? " (x2apic mode, msr access)\r\n"
                               : ((apic_base_msr & (1ULL << 10)) ? " (x2apic mode ON)\r\n" : " (xapic mode)\r\n"));
    {   // hybrid part? leaf 0x1a reports the calling core's type (0x20 atom, 0x40 core). (satoru)
        uint32_t a, b, c, d;
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0u), "c"(0u));
        if (a >= 0x1A) {
            __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x1Au), "c"(0u));
            if (a >> 24) {
                SerialLogger::Log("[SMP] hybrid cpu, bsp core type ");
                SerialLogger::Log((a >> 24) == 0x40 ? "P (core)\r\n" : "E (atom)\r\n");
            }
        }
    }

    // acpi root: the bootloader-provided rsdp first (the only source on uefi),
    // then the legacy bios scan. (satoru)
    uint64_t rsdp = BootAcpi::Rsdp();
    bool rsdp_boot = rsdp != 0;
    if (rsdp) {
        SerialLogger::Log(BootAcpi::IsEfiBoot() ? "[ACPI] rsdp from efi at " : "[ACPI] rsdp from boot tag at ");
        SerialLogger::LogHex64(rsdp); SerialLogger::Log("\r\n");
    } else {
        rsdp = find_rsdp();
        if (rsdp) { SerialLogger::Log("[ACPI] rsdp from legacy scan at "); SerialLogger::LogHex64(rsdp); SerialLogger::Log("\r\n"); }
        else SerialLogger::Log("[ACPI] no rsdp found\r\n");
    }
    (void)rsdp_boot;
    const AcpiSdt* madt = rsdp ? find_madt(rsdp) : nullptr;

    // cpu cap: explicit kurono.maxcpus wins; else 8 on efi boots (real hardware has
    // only ever run stably with few cores); bios/qemu paths stay uncapped. (satoru)
    uint32_t cap = SMP_MAX_CPUS;
    bool cap_default = false;
    if (g_max_cpus_req) cap = g_max_cpus_req;
    // sched=2 lifts the efi default: every cpu the madt lists, up to
    // SMP_MAX_CPUS. kurono.maxcpus still wins. (satoru)
    else if (BootAcpi::IsEfiBoot() && !Scheduler::SchedV2()) { cap = 8; cap_default = true; }
    if (cap > SMP_MAX_CPUS) cap = SMP_MAX_CPUS;
    if (cap < 1) cap = 1;
    // x2apic locked on: the aps come up through the msr icr (icr_send) and enable
    // x2apic themselves in ap_entry, so the cap no longer applies. a dead lapic
    // still means one cpu. (satoru)
    if (g_lapic_dead) cap = 1;

    uint32_t available = 0;
    if (madt) {
        parse_madt(madt, bsp_apic);
        available = g_cand_n;
        register_cpu(bsp_apic);
        // madt order: on hybrid parts firmware lists the p-cores first, so first-listed
        // prefers them (leaf 0x1a is only readable on the cpu itself). (satoru)
        for (uint32_t i = 0; i < g_cand_n && g_cpu_count < cap; i++)
            if (g_cand[i] != bsp_apic) register_cpu(g_cand[i]);
        SerialLogger::Log("[SMP] MADT found: ");
    } else {
        // fallback: trust the cpuid logical-core count, assume sequential apic ids
        // 0..n-1 with the bsp first. (satoru)
        register_cpu(bsp_apic);
        int n = CPUDetect::GetThreadCount();
        if (n < 1) n = 1;
        available = (uint32_t)n;
        for (int id = 0; id < n && g_cpu_count < cap; id++)
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
    if (available > g_cpu_count) {
        SerialLogger::Log("[SMP] capped at "); SerialLogger::LogDec((int)cap);
        SerialLogger::Log(cap_default ? " (default on efi; kurono.maxcpus=N overrides), " : " (kurono.maxcpus), ");
        SerialLogger::LogDec((int)available); SerialLogger::Log(" available\r\n");
    }
    if (available > SMP_MAX_CPUS)
        SerialLogger::Log("[SMP] WARNING: more cpus than SMP_MAX_CPUS, extra ignored\r\n");
}

uint32_t SMP::CpuCount() { return g_cpu_count ? g_cpu_count : 1; }
uint64_t SMP::KernelStackBytes() { return g_ap_stack_bytes; }

uint32_t SMP::OnlineCount() {
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_cpu_count; i++) if (g_cpus[i].online) n++;
    return n ? n : 1;
}

void SMP::MarkSelfOffline() {
    // containment park (see smp.h): stop counting this cpu for tlb-ack wants
    // and the online census - it is about to cli-hlt forever. (satoru)
    uint32_t idx = CpuIndex();
    if (idx < g_cpu_count) g_cpus[idx].online = 0;
}

uint32_t SMP::ApicId() {
    if (g_lapic_base == 0 || g_lapic_dead) return 0;
    if (g_x2apic) return LapicRead(0x20);   // x2apic id msr is the full 32 bits (satoru)
    return LapicRead(0x20) >> 24;   // local apic id register (satoru)
}

uint32_t SMP::CpuIndex() {
    if (g_tscaux) {
        // tsc_aux holds index+1 once this cpu programmed it; 0 = not yet (an ap
        // in its first instructions), fall back to the lapic. (satoru)
        uint32_t lo, hi, aux;
        __asm__ volatile("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux));
        (void)lo; (void)hi;
        if (aux && aux <= g_cpu_count) return aux - 1;
    }
    int ix = apicid_index(ApicId());
    if (ix >= 0) return (uint32_t)ix;
    return 0;  // unknown -> treat as bsp (satoru)
}

PerCpu* SMP::Current()            { return &g_cpus[CpuIndex()]; }
PerCpu* SMP::ByIndex(uint32_t i)  { return (i < g_cpu_count) ? &g_cpus[i] : nullptr; }

uint32_t SMP::LapicRead(uint32_t reg) {
    if (g_lapic_dead) return 0;
    if (g_x2apic) return reg == 0x310 ? 0u : (uint32_t)rdmsr(0x800u + (reg >> 4));
    return mmio_r32(g_lapic_base + reg);
}
void SMP::LapicWrite(uint32_t reg, uint32_t v) {
    if (g_lapic_dead) return;
    if (g_x2apic) {
        // x2apic folds the two xapic icr halves into one 64-bit msr (0x830), destination in
        // bits 63:32 instead of 31:24 of the high half. (satoru)
        if (reg == 0x310) { g_icr_hi = v; return; }
        if (reg == 0x300) { wrmsr(0x830, ((uint64_t)(g_icr_hi >> 24) << 32) | v); g_icr_hi = 0; return; }
        wrmsr(0x800u + (reg >> 4), v);
        return;
    }
    mmio_w32(g_lapic_base + reg, v);
}

void SMP::SetupGsBase() {
    // KERNEL_GS_BASE holds this cpu's PerCpu pointer while in user mode; the
    // SYSCALL stub does swapgs at entry to bring it into gs, reads gs:0/gs:8, and
    // swapgs back before iretq - net-zero per syscall, so it permanently holds the
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
//  no shared serial here - the bsp logs each core after its online flag flips,
//  so there is no cross-core console race. (satoru)
extern "C" void ap_entry() {
    // before this cpu reads time: make its rdtsc agree with the bsp's (see
    // tsc_adjust_sanitize_this_cpu). the value is logged by the bsp. (satoru)
    int64_t fw_tsc_adjust = tsc_adjust_sanitize_this_cpu();
    // match the bsp's mode before the first lapic mmio access: an ap that comes out of
    // INIT-SIPI with x2apic still on (firmware left it) would hit the dead window. no
    // serial here (shared console); the bsp reports the outcome via the online flag. (satoru)
    // with the bsp in x2apic mode (locked by firmware, or kurono.forcex2apic) the ap
    // must be in x2apic mode too: the msr block is the only lapic interface then, and
    // a locked x2apic cannot be turned off anyway. the firmware usually hands the aps
    // over in x2apic already (INIT keeps the mode); otherwise enable it here, EN
    // before EXTD (xapic -> x2apic is the legal transition), before any lapic access. (satoru)
    if (g_x2apic) {
        uint64_t ab = rdmsr(0x1B);
        if (!(ab & (1ULL << 11))) { ab |= (1ULL << 11); wrmsr(0x1B, ab); }
        if (!(ab & (1ULL << 10))) wrmsr(0x1B, ab | (1ULL << 10));
    } else {
        (void)x2apic_to_xapic();
    }
    // same pat as the bsp (kurono_boot.asm): pat1 = write-combining, so a wc
    // framebuffer mapping is wc on this cpu too, not the reset default (write-
    // through). before any framebuffer access from this cpu. (satoru)
    {
        uint64_t pat = rdmsr(0x277);
        pat = (pat & ~0xFF00ULL) | 0x0100ULL;
        wrmsr(0x277, pat);
    }
    SMP::LapicEnable();
    // sched=2 fast CpuIndex: program this cpu's tsc_aux = index+1 BEFORE anything
    // calls CpuIndex (SetupAPCpuState does, right below). the index comes from
    // the lapic map directly. (satoru)
    if (g_tscaux) {
        int ix = apicid_index(SMP::ApicId());
        wrmsr(MSR_TSC_AUX, ix >= 0 ? (uint64_t)(ix + 1) : 0);
    }
    HAL::SetupAPCpuState();  // own gdt+tss, shared idt, per-core syscall msrs (satoru)
    SMP::SetupGsBase();      // KERNEL_GS_BASE -> this ap's PerCpu (after the gs reload) (satoru)
    PerCpu* me = SMP::Current();
    {
        uint32_t ix = SMP::CpuIndex();
        if (ix < SMP_MAX_CPUS) g_tscadj_boot[ix] = fw_tsc_adjust;   // before online: the bsp reads it then (satoru)
    }
    SMP::ApplyCrPolicyAp();   // same cr0.wp/ne + cr4 smep/umip as the bsp, before online: the bsp logs it (satoru)
    if (me) me->online = 1;

    //  phase 4 - arm a per-ap LAPIC timer (vector 0x40, periodic) so user threads
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
        // ~100 hz. (tried 1 khz to tighten thread handshakes - firefox stalled
        // right after its thread-spawn burst: the 10x preempt rate on the aps
        // widened a save/resume race window. back to the proven 10 ms grain.)
        // (satoru)
        uint32_t period = ticks_10ms ? ticks_10ms : 1000000u;   // ticks in 10 ms (satoru)
        { uint32_t ix = SMP::CpuIndex(); if (ix < SMP_MAX_CPUS) g_lapic_10ms[ix] = period; }
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
//     multi-threaded process and iretq into its saved frame - from then on the
//     per-ap LAPIC timer (ApTimerPreempt) multiplexes this cpu across the ready
//     threads, giving the process TRUE parallelism with the bsp.
//  2) phase 3d fresh-proc launch (kurono.apsched): claim an explicitly-pinned
//     never-entered user process and run it via RunProcessWithArgs. (satoru)
extern "C" [[noreturn]] void ap_dispatch_reenter() {
    uint32_t idx = SMP::CpuIndex();
    // sched=2: drop the last task's cr3 before current goes null - a launcher
    // that sees this cpu idle may destroy that address space at once. (satoru)
    if (Scheduler::SchedV2()) SMP::LoadKernelCr3();
    Scheduler::SetCurrentForThisCpu(nullptr);   // no task on this cpu right now (satoru)
    Userspace::SetActiveForThisCpu(nullptr);    // no active user session either (satoru)
    //  idle maintenance throttle (task 17): a parked cpu must still advance the
    //  sleep_ticks promotions + the futex repoll heal, or threads it parked
    //  (futex/nanosleep with no successor) oversleep until the bsp's next
    //  cooperative Schedule (~50ms) - the promotion paths in the timer preempts
    //  are ring-3-gated and never run while this cpu idles in ring-0 here. (satoru)
    uint64_t last_maint_ms = 0;
    for (;;) {
        //  never idle on a dead task's cr3: this cpu owns no task here, but its
        //  last thread's address space may be reaped + its page tables recycled
        //  while our walker can still fetch through them (garbage kernel
        //  translations = random corruption). cheap: compares before writing. (satoru)
        SMP::LoadKernelCr3();
        // sched=2: this loop runs on the ap's own bring-up stack, so any task the
        // cpu just left (park, exit, kill divert) is off-stack now - publish its
        // on_cpu=0 so another cpu may resume or reap it. (satoru)
        Scheduler::FinishSwitch();
        // sched=2: point this cpu's rsp0 / ist1 / gs:8 back at its own stack (its
        // bring-up state). they still named the kernel stack of the last task it
        // ran, which kept a reaped task's stack from ever being freed - and an
        // interrupt through ist1 would have landed on a freed stack. (satoru)
        if (Scheduler::SchedV2()) { PerCpu* mc = SMP::Current(); if (mc && mc->kernel_stack_top && !Userspace::SessionIdleRsp(idx)) HAL::SetKernelStack(mc->kernel_stack_top); }
        // kurono.svccpu: this ap hosts kernel procs (the desktop, the media and
        // network services, the vm host) and never a user task. it leaves the
        // dispatch loop for good, before it could claim the boot animation job:
        // the gui ends that job, and the gui may live right here. (satoru)
        if (Scheduler::IsServiceCpu(idx)) Scheduler::ServiceCpuLoop();
        // boot animation job: one ap draws the emblem while the bsp boots; it
        // returns here once the gui process takes the screen over. (satoru)
        if (BootAnim::ApPending()) BootAnim::ApService();
        {
            uint64_t nowm = Timer::GetRealMs64();
            if (nowm != last_maint_ms) {   // at most once per ms (satoru)
                last_maint_ms = nowm;
                Scheduler::ApIdleMaint();
            }
        }
        if (g_ap_thread_sched) {
            Process* t = Scheduler::ClaimReadyThreadForCpu(idx);
            // (the apsched fresh-launch source below still polls, so it keeps the
            // old spin when that gate is on.) (satoru)
            if (!t && Scheduler::SchedV2() && !g_ap_user_sched) {
                // sched=2 idle: publish "halted" with interrupts off, look once
                // more, then sti;hlt (the sti shadow makes the pair atomic, so a
                // kick that lands after the flag is set wakes the hlt instead of
                // being taken before it). the lapic tick still wakes us every
                // 10ms for the idle maintenance above. (satoru)
                __asm__ volatile("cli" ::: "memory");
                __atomic_store_n(&g_cpu_idle[idx], (uint8_t)1, __ATOMIC_SEQ_CST);
                t = Scheduler::ClaimReadyThreadForCpu(idx);
                if (!t) __asm__ volatile("sti; hlt" ::: "memory");
                __atomic_store_n(&g_cpu_idle[idx], (uint8_t)0, __ATOMIC_RELEASE);
                __asm__ volatile("sti" ::: "memory");
                if (!t) continue;
            }
            if (t) {
                //  claim trace - THROTTLED: with the futex/nanosleep ap-park a
                //  resume happens per wake (thousands/boot) and one serial line
                //  is an ~8.7ms uart busy-wait - unthrottled this alone dragged
                //  the boot by tens of seconds. first few + 1-per-4096. (satoru)
                static uint32_t s_apr_n = 0;
                uint32_t an = __atomic_add_fetch(&s_apr_n, 1u, __ATOMIC_RELAXED);
                if (an <= 12 || (an & 4095u) == 0) {
                    uint64_t lf; g_ap_log_lock.LockIrqSave(&lf);
                    SerialLogger::Log("[apr] cpu"); SerialLogger::LogDec((int)idx);
                    SerialLogger::Log(" resume pid="); SerialLogger::LogDec((int)t->pid);
                    SerialLogger::Log(" n="); SerialLogger::LogDec((int)an);
                    SerialLogger::Log("\r\n");
                    g_ap_log_lock.UnlockIrqRestore(lf);
                }
                //  resume the thread from its saved user frame. LoadUserFrame sets
                //  this cpu's current task, tss.rsp0 + gs:8, cr3, fs base and fpu;
                //  the asm pivot then irets into ring-3. never returns - the LAPIC
                //  timer preempt keeps this cpu multiplexing ready threads, and a
                //  thread exit with nothing left claimable irets back here. (satoru)
                InterruptFrame f;
                //  a kill that landed after the claim dropped the lock: do not enter it. only
                //  here - ScheduleNextUser has already released its cur before it loads, so a
                //  false from LoadUserFrame there would leave cur running unowned (the review
                //  11-07 early return did exactly that; one firefox #GP). (satoru)
                if (__atomic_load_n((int*)&t->state, __ATOMIC_ACQUIRE) != (int)Process_Terminated &&
                    Scheduler::LoadUserFrame(t, &f)) {
                    //  the per-cpu userspace-active gate: without it IsActive() is
                    //  false on this ap and the whole linux syscall layer no-ops
                    //  (no frame save, no linux current, sys_exit does nothing - 
                    //  the thread's exit loop spins forever). (satoru)
                    Userspace::SetActiveForThisCpu(t);
                    //  sync this cpu's linux current-process to the resumed thread,
                    //  or LinuxSyscall::Current() is null on the ap and syscalls
                    //  (mremap stack-probe) loop on ENOMEM (the [eag] pid=-1 spam).
                    //  (satoru)
                    LinuxSyscall::SyncCurrentToTask(t);
                    //  ap_enter_user_frame irets WITHOUT swapgs, so set the active
                    //  gs to the thread's user gs (and park the per-cpu ptr in
                    //  KERNEL_GS_BASE) - the no-swapgs form, same as the isr
                    //  preempt path. (satoru)
                    Scheduler::FixupGsAfterIsrSwitch();
                    ap_enter_user_frame(&f);
                }
                //  load failed (no saved frame?) - put it back. (satoru)
                {   // put it back - unless a kill landed meanwhile (audit 11-07 #1) (satoru)
                    int st = (int)Process_Running;
                    __atomic_compare_exchange_n((int*)&t->state, &st, (int)Process_Ready, false,
                                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
                }
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
                Scheduler::EndApLaunch(idx);   // agent_sched_10 (satoru)
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

void SMP::EnableFastCpuIndex() {
    if (!Scheduler::SchedV2()) return;
    uint32_t a, b, c, d;
    cpuid_ex(0x80000000u, 0, &a, &b, &c, &d);
    if (a < 0x80000001u) return;
    cpuid_ex(0x80000001u, 0, &a, &b, &c, &d);
    if (!(d & (1u << 27))) {   // no rdtscp (satoru)
        SerialLogger::Log("[sched2] no rdtscp, CpuIndex stays on the lapic\r\n");
        return;
    }
    wrmsr(MSR_TSC_AUX, 1);   // the bsp is index 0 (satoru)
    g_tscaux = true;
    SerialLogger::Log("[sched2] CpuIndex via tsc_aux (rdtscp)\r\n");
}

// send one fixed ipi to a cpu by dense index. cli across the two icr writes so
// an interrupt on this cpu cannot slip its own icr write in between, and wait
// for delivery before returning so the next icr user (a tlb broadcast) never
// overwrites a pending send. (satoru)
static void send_fixed_ipi(uint32_t idx, uint8_t vec) {
    uint64_t fl;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(fl) :: "memory");
    icr_idle();
    icr_send(g_cpus[idx].apic_id, (1u << 14) | vec);   // fixed, assert, physical (satoru)
    icr_idle();
    if (fl & 0x200) __asm__ volatile("sti" ::: "memory");
}

bool SMP::DebugIdle(uint32_t idx) { return idx < SMP_MAX_CPUS && g_cpu_idle[idx]; }

void SMP::KickCpu(uint32_t idx) {
    if (!Scheduler::SchedV2() || idx >= g_cpu_count || idx == CpuIndex() || !g_cpus[idx].online) return;
    send_fixed_ipi(idx, 0x42);
}

void SMP::KickOneIdle() {
    if (!Scheduler::SchedV2() || g_cpu_count < 2) return;
    static volatile uint32_t s_rr = 0;
    uint32_t me = CpuIndex();
    uint32_t start = __atomic_fetch_add(&s_rr, 1u, __ATOMIC_RELAXED);
    for (uint32_t k = 0; k < g_cpu_count; k++) {
        uint32_t c = 1u + (start + k) % (g_cpu_count - 1u);   // aps only (satoru)
        if (c == me || !g_cpus[c].online) continue;
        if (Scheduler::IsServiceCpu(c)) continue;   // runs no user task (satoru)
        uint8_t one = 1;
        if (!__atomic_compare_exchange_n(&g_cpu_idle[c], &one, (uint8_t)0, false,
                                         __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) continue;
        send_fixed_ipi(c, 0x42);
        return;
    }
}

// retune the calling ap's periodic lapic timer (vector 0x40) from the 10 ms
// calibration ap_entry made. a service cpu runs at 1 khz so its kernel procs'
// SleepMs(1) means 1 ms, as on cpu 0's pit. (satoru)
void SMP::SetLapicTickHz(uint32_t hz) {
    uint32_t ix = CpuIndex();
    if (ix == 0 || ix >= SMP_MAX_CPUS || !hz || !g_lapic_10ms[ix]) return;
    uint64_t c = (uint64_t)g_lapic_10ms[ix] * 100u / hz;
    if (c < 16) c = 16;
    LapicWrite(0x320, 0x40u | (1u << 17));   // vector 0x40, periodic (satoru)
    LapicWrite(0x380, (uint32_t)c);
}

// bsp tick from the lapic timer (1112 laptop panic). with 20 cpus online cpu0 took
// interrupts on vectors nothing sends: 0 (eight times in the first 10 s, each with
// the 8259's irq0 in service), then 0xa9 (#gp on the empty gate) or 4 (#OF), always
// halted in SleepMs. with one cpu online, on the same machine, never. the pit tick
// reached cpu0 as extint through lint0, the only extint user, and with other cpus
// sending ipis that legacy inta path returns a wrong vector. no mainstream kernel
// drives a modern machine like that (linux masks the 8259 and ticks from the lapic
// timer). so with 2+ cpus the bsp ticks from its own lapic timer on vector 32, the
// pit irq0 is masked at the 8259, and the vector 32 path is unchanged (pit_ticks,
// OnTimerTick, the kls preempt hook) apart from the lapic eoi. the pit still counts
// for Timer::ReadCounter. needs arat (the timer runs in every c-state) or a
// hypervisor, else the pit stays. (satoru)
static volatile bool g_bsp_lapic_tick = false;
bool SMP::BspLapicTickActive() { return g_bsp_lapic_tick; }
void SMP::TestSelfIpi(uint8_t vec) {
    uint32_t me = CpuIndex();
    if (g_lapic_dead || me >= g_cpu_count) return;
    icr_send(g_cpus[me].apic_id, (1u << 14) | vec);   // fixed, assert, physical (satoru)
}

void SMP::StartBspLapicTick(int mode) {
    if (mode < 0) {
        SerialLogger::Log("[tick] bsp: pit through the 8259 (kurono.bsptick=pit)\r\n");
        return;
    }
    if (g_lapic_dead || g_lapic_base == 0) return;
    if (mode == 0 && OnlineCount() < 2) return;   // one cpu: no ipis, the pit as before (satoru)
    uint32_t a, b, c, d;
    cpuid_ex(6, 0, &a, &b, &c, &d);
    bool arat = (a >> 2) & 1;
    if (!arat && !CPUDetect::IsHypervisor()) {
        SerialLogger::Log("[tick] bsp: no arat, the lapic timer may stop when idle: keeping the pit\r\n");
        return;
    }
    uint32_t tpr = LapicRead(0x80);
    if ((tpr & 0xF0) >= 0x20) {
        // a task priority at or above class 2 would hold vector 32 back. (satoru)
        SerialLogger::Log("[tick] bsp: tpr was 0x"); SerialLogger::LogHex(tpr); SerialLogger::Log(", now 0\r\n");
        LapicWrite(0x80, 0);
    }
    // calibrate as ap_entry does: divide by 16, masked one-shot from the top, 10 ms. (satoru)
    LapicWrite(0x3E0, 0x3);
    LapicWrite(0x320, (1u << 16));
    LapicWrite(0x380, 0xFFFFFFFFu);
    busy_us(10000);
    uint32_t t10 = 0xFFFFFFFFu - LapicRead(0x390);
    LapicWrite(0x380, 0);
    if (t10 < 1000u) {   // under 100 khz: not believable, keep the pit (satoru)
        SerialLogger::Log("[tick] bsp: lapic timer calibration failed (");
        SerialLogger::LogDec((int)t10); SerialLogger::Log(" in 10 ms): keeping the pit\r\n");
        return;
    }
    uint32_t per_ms = t10 / 10u;
    g_bsp_lapic_tick = true;                     // before the first tick: it needs the lapic eoi (satoru)
    LapicWrite(0x320, 0x20u | (1u << 17));       // vector 32, periodic (satoru)
    LapicWrite(0x380, per_ms);
    HAL::DisableIRQ(0);                          // pit irq0 off at the 8259 (satoru)
    SerialLogger::Log("[tick] bsp: 1 khz from the lapic timer on vector 32 (");
    SerialLogger::LogDec((int)per_ms);
    SerialLogger::Log(" counts/ms), pit irq0 masked at the 8259");
    SerialLogger::Log(arat ? "" : " (no arat, hypervisor)");
    SerialLogger::Log("\r\n");
}

// like KickOneIdle, but only an idle ap that may run `p` (affinity, the clone pin, a
// leader's session cpu): waking one that cannot claim it just re-halts it while the cpu
// that could sleeps to its next 10 ms tick (audit 11-07 #5). no eligible idle ap = no
// kick (a busy eligible cpu picks it up at its next pick; the bsp handles itself). (satoru)
void SMP::KickIdleFor(const Process* p) {
    if (!p) { KickOneIdle(); return; }
    if (!Scheduler::SchedV2() || g_cpu_count < 2) return;
    static volatile uint32_t s_rr2 = 0;
    uint32_t me = CpuIndex();
    uint32_t start = __atomic_fetch_add(&s_rr2, 1u, __ATOMIC_RELAXED);
    for (uint32_t k = 0; k < g_cpu_count; k++) {
        uint32_t c = 1u + (start + k) % (g_cpu_count - 1u);   // aps only (satoru)
        if (c == me || !g_cpus[c].online) continue;
        if (!Scheduler::CpuMayRun(p, c)) continue;
        uint8_t one = 1;
        if (!__atomic_compare_exchange_n(&g_cpu_idle[c], &one, (uint8_t)0, false,
                                         __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) continue;
        send_fixed_ipi(c, 0x42);
        return;
    }
}

void SMP::SetApUserSched(bool on) { g_ap_user_sched = on; }
bool SMP::ApUserSched()           { return g_ap_user_sched; }

void SMP::SetApThreadSched(bool on) { g_ap_thread_sched = on; }
bool SMP::ApThreadSched()           { return g_ap_thread_sched; }

//  ring-0 reentry frame into the dispatch loop, for the syscall exit path: the
//  ap's current thread just exited and nothing else is claimable, so the
//  handler's iretq must land somewhere - back at the top of the claim loop, on
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
    // THE lockB rip=0x1 CRASH: under kurono.apsched the leader's
    // RunProcessWithArgs launcher runs ON this ap's own stack (the dispatch loop
    // called it). when a sibling thread exits or parks here, the cpu irets into
    // the dispatch loop at the TOP of that same stack and overwrites the live
    // launcher frame; the leader's later exit longjmps through the wreck. run
    // the loop below the launcher frame instead. (satoru)
    if (uint64_t srsp = Userspace::SessionIdleRsp(CpuIndex())) f->rsp = srsp;
    f->rflags = 0x202ULL;
}

//  tlb shootdown. sender: pulse vector 0x41 at every other cpu (all-excluding-
//  self shorthand) and wait - bounded - for each online peer to ack. bounded
//  because a peer may briefly sit with IF=0 (a spinlock hold); the per-ap timer
//  cr3 reload in ApTimerPreempt bounds any missed window to one tick. (satoru)
// sched=2 broadcast: a cpu halted in its dispatch loop sits on the kernel cr3
// (LoadKernelCr3 runs before every idle check) and holds no user translation, so
// it needs no flush - linux's lazy-tlb idea. waiting for halted vcpus to wake and
// ack was the main cost of every munmap/mprotect once idle aps halt (sampled:
// the kls holder in BroadcastTlbFlush while every other cpu spun on kls). a cpu
// counts as done when it acked this round OR is observed halted. (satoru)
// a cpu waiting for the broadcast lock may have IF=0 (cow / demand-zero quarantine
// flushes from #PF, the kstack guard install), so it cannot take the holder's 0x41
// ipi and, not being idle, held that round up for the whole 20 ms timeout - and a
// timed-out round never advances the full-flush epoch, so quarantined frames piled
// up (audit 11-07 #10). the waiter services a pending round itself: reload cr3 and
// ack it, with IF off so the real ipi cannot interleave on this cpu. sched=2 only:
// its acks are per-cpu round numbers read before the reload, so a late or repeated
// ack can never count toward a newer round. the old mode's shared ack COUNTER cannot
// tell rounds apart - an inline ack read for round N could land after the next
// holder's reset and count for N+1 (review finding) - so it keeps the old spin. (satoru)
static void tlb_service_pending_inline_v2() {
    uint32_t me = SMP::CpuIndex();
    if (me >= SMP_MAX_CPUS) return;
    uint64_t fl;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(fl) : : "memory");
    uint64_t s = __atomic_load_n(&g_tlb_flush_start_seq, __ATOMIC_ACQUIRE);
    if (s > __atomic_load_n(&g_tlb_acked_seq[me], __ATOMIC_RELAXED)) {
        uint64_t c3v;
        __asm__ volatile("mov %%cr3, %0" : "=r"(c3v));
        __asm__ volatile("mov %0, %%cr3" : : "r"(c3v) : "memory");
        __atomic_store_n(&g_tlb_acked_seq[me], s, __ATOMIC_RELEASE);
    }
    if (fl & 0x200) __asm__ volatile("sti" : : : "memory");
}

static void broadcast_tlb_flush_v2() {
    static volatile uint32_t s_bcast_lock2 = 0;
    while (__atomic_exchange_n(&s_bcast_lock2, 1u, __ATOMIC_ACQUIRE)) {
        tlb_service_pending_inline_v2();   // audit 11-07 #10 (satoru)
        __asm__ volatile("pause");
    }
    uint64_t seq = __atomic_add_fetch(&g_tlb_flush_start_seq, 1u, __ATOMIC_SEQ_CST);
    uint32_t me = SMP::CpuIndex();
    for (int i = 0; i < 100000 && (SMP::LapicRead(0x300) & (1u << 12)); i++)
        __asm__ volatile("pause");
    icr_send(0, (3u << 18) | (1u << 14) | 0x41u);   // shorthand: dest unused (satoru)
    uint64_t t0 = Timer::GetRealMs64();
    for (;;) {
        bool all = true;
        for (uint32_t c = 0; c < g_cpu_count && all; c++) {
            if (c == me || !g_cpus[c].online) continue;
            if (__atomic_load_n(&g_tlb_acked_seq[c], __ATOMIC_ACQUIRE) >= seq) continue;
            if (__atomic_load_n(&g_cpu_idle[c], __ATOMIC_ACQUIRE)) continue;
            all = false;
        }
        if (all) {
            uint64_t cur = __atomic_load_n(&g_tlb_flush_full_seq, __ATOMIC_ACQUIRE);
            while (cur < seq &&
                   !__atomic_compare_exchange_n(&g_tlb_flush_full_seq, &cur, seq,
                                                false, __ATOMIC_SEQ_CST,
                                                __ATOMIC_ACQUIRE)) { }
            break;
        }
        if (Timer::GetRealMs64() - t0 > 20) {
            __atomic_fetch_add(&g_tlb_flush_timeouts, 1u, __ATOMIC_RELAXED);
            break;
        }
        __asm__ volatile("pause");
    }
    __atomic_store_n(&s_bcast_lock2, 0u, __ATOMIC_RELEASE);
}

void SMP::BroadcastTlbFlush() {
    uint32_t online = OnlineCount();
    if (online <= 1) return;
    if (Scheduler::SchedV2()) { broadcast_tlb_flush_v2(); return; }
    //  serialize broadcasters: the kls historically serialized every caller,
    //  but the kmemx pending-free flushes from its own kernel process, so two
    //  rounds could share one ack counter - a peer's double-ack (collapsed
    //  ipis) can then stand in for a peer that never flushed, falsely
    //  advancing the proof epoch. plain exchange spin with IF ON: a waiting
    //  broadcaster must still service + ack the holder's ipi or the holder
    //  times out on it. (satoru)
    static volatile uint32_t s_bcast_lock = 0;
    while (__atomic_exchange_n(&s_bcast_lock, 1u, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
    uint64_t seq = __atomic_add_fetch(&g_tlb_flush_start_seq, 1u, __ATOMIC_SEQ_CST);
    uint32_t want = online - 1;
    __atomic_store_n(&g_tlb_ack, 0u, __ATOMIC_RELEASE);
    //  wait for any prior ipi to leave the icr (delivery status, bit 12). (satoru)
    for (int i = 0; i < 100000 && (LapicRead(0x300) & (1u << 12)); i++)
        __asm__ volatile("pause");
    //  fixed delivery, assert, destination shorthand 11 = all excluding self. (satoru)
    icr_send(0, (3u << 18) | (1u << 14) | 0x41u);   // shorthand: dest unused (satoru)
    //  ack wait: the old ~sub-ms pause bound silently missed any peer holding
    //  IF=0 longer than that - and a single serial line is an ~8.7ms ring-0
    //  uart busy-wait. a peer that misses the ipi keeps a STALE translation
    //  while the caller (mprotect/munmap) believes the flush completed; for the
    //  jit's W^X flips that is another core executing stale code bytes = the
    //  per-boot-random-rip #GP crashes, and for a recycled frame it is one
    //  thread's writes spraying another thread's memory (the
    //  fontconfig-string-over-stack-canary corruption). wait ~20ms wall clock;
    //  still bounded so a wedged peer cannot hang the unmap path. this call can
    //  itself run from an IF=0 exception context (a cow/demand-zero fault that
    //  quarantines a frame), so do NOT retry-loop or serial-log here - that
    //  would extend the IF=0 window and cascade OTHER cores' acks into timeout.
    //  on success, latch the flush epoch so the quarantine may release the
    //  frames whose ptes were cleared before this broadcast. (satoru)
    uint64_t t0 = Timer::GetRealMs64();
    for (;;) {
        if (__atomic_load_n(&g_tlb_ack, __ATOMIC_ACQUIRE) >= want) {
            uint64_t cur = __atomic_load_n(&g_tlb_flush_full_seq, __ATOMIC_ACQUIRE);
            while (cur < seq &&
                   !__atomic_compare_exchange_n(&g_tlb_flush_full_seq, &cur, seq,
                                                false, __ATOMIC_SEQ_CST,
                                                __ATOMIC_ACQUIRE)) { }
            __atomic_store_n(&s_bcast_lock, 0u, __ATOMIC_RELEASE);
            return;
        }
        if (Timer::GetRealMs64() - t0 > 20) break;
        __asm__ volatile("pause");
    }
    //  timed out - a peer had interrupts masked >20ms; its next timer tick
    //  reloads cr3. the epoch is NOT advanced, so the quarantine holds any
    //  frame cleared before this broadcast until a LATER fully-acked flush -
    //  the stale window cannot recycle memory. counter only (no serial i/o in
    //  this hot, possibly-IF=0 path). (satoru)
    __atomic_fetch_add(&g_tlb_flush_timeouts, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_bcast_lock, 0u, __ATOMIC_RELEASE);
}

// task 21: expose the silent shootdown-timeout counter so the wedge probe can
// correlate timed-out flushes (stale peers + immediate va reuse) with the
// lost-write wedge. (satoru)
uint32_t SMP::TlbFlushTimeouts() {
    return __atomic_load_n(&g_tlb_flush_timeouts, __ATOMIC_RELAXED);
}

void SMP::HandleTlbIpi() {
    DbgWatch::ApplyThisCpu();   // kurono.dbgwatch rides the shootdown ipi (satoru)
    if (Scheduler::SchedV2()) {
        uint64_t s = __atomic_load_n(&g_tlb_flush_start_seq, __ATOMIC_ACQUIRE);
        uint64_t c3v;
        __asm__ volatile("mov %%cr3, %0" : "=r"(c3v));
        __asm__ volatile("mov %0, %%cr3" : : "r"(c3v) : "memory");
        uint32_t me = CpuIndex();
        if (me < SMP_MAX_CPUS) {
            uint64_t old = __atomic_load_n(&g_tlb_acked_seq[me], __ATOMIC_RELAXED);
            if (s > old) __atomic_store_n(&g_tlb_acked_seq[me], s, __ATOMIC_RELEASE);
        }
        return;
    }
    uint64_t c3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(c3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(c3) : "memory");
    __atomic_fetch_add(&g_tlb_ack, 1u, __ATOMIC_ACQ_REL);
}

void SMP::LoadKernelCr3() {
    uint64_t k = __atomic_load_n(&g_kernel_cr3, __ATOMIC_ACQUIRE);
    if (!k) return;
    uint64_t c3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(c3));
    if (c3 == k) { KernelVMM::NoteCr3(k); return; }
    __asm__ volatile("mov %0, %%cr3" : : "r"(k) : "memory");
    KernelVMM::NoteCr3(k);   // [asdead] (satoru)
}

uint64_t SMP::TlbFlushStartSeq() {
    return __atomic_load_n(&g_tlb_flush_start_seq, __ATOMIC_SEQ_CST);
}

uint64_t SMP::TlbFlushFullSeq() {
    return __atomic_load_n(&g_tlb_flush_full_seq, __ATOMIC_SEQ_CST);
}

void SMP::StartAPs() {
    // capture the kernel root for LoadKernelCr3 BEFORE the single-cpu early
    // return - the bsp's nested-fault park uses it too. StartAPs runs during
    // kernel boot, so the active cr3 here IS the kernel address space. (satoru)
    {
        uint64_t kc3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(kc3));
        // the ap trampoline loads cr3 with a 32-bit `mov cr3,eax` before it
        // enables long mode, so the kernel root itself must live below 4 gb.
        // RelocateKernelRoot copies the tables wherever the pmm likes; move
        // the root again (the tables under it may stay high) if needed. (satoru)
        kc3 = KernelVMM::EnsureRootBelow4GB();
        g_kernel_cr3 = kc3;
    }
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

    // patch area at 0x9000 - fields the trampoline reads on its way up. (satoru)
    volatile uint64_t* p_cr3   = (volatile uint64_t*)(uintptr_t)0x9000;
    volatile uint8_t*  p_gdt   = (volatile uint8_t*) (uintptr_t)0x9008;
    volatile uint64_t* p_entry = (volatile uint64_t*)(uintptr_t)0x9018;
    volatile uint64_t* p_stack = (volatile uint64_t*)(uintptr_t)0x9020;

    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    if (cr3 >> 32) {
        // no below-4gb root could be allocated: do not hand the trampoline a
        // truncated cr3 (that would walk random tables). boot uniprocessor.
        SerialLogger::Log("[SMP] kernel root 0x");
        SerialLogger::LogHex64(cr3);
        SerialLogger::Log(" is above 4 gb, AP trampoline disabled\r\n");
        return;
    }
    *p_cr3 = cr3;

    struct __attribute__((packed)) { uint16_t limit; uint64_t base; } gdtr;
    __asm__ volatile("sgdt %0" : "=m"(gdtr));
    // the trampoline does lgdt in 32-bit mode before the far jump, so the base
    // field must be the physical address of the gdt. the gdt lives in the
    // high-linked kernel image now, so it needs the image translation. the
    // identity map still maps that physical page, which is what makes the
    // truncated 32-bit lgdt operand valid. (satoru)
    gdtr.base = KernelPhysOf((const void*)(uintptr_t)gdtr.base);
    const uint8_t* g = (const uint8_t*)&gdtr;
    for (int k = 0; k < 10; k++) p_gdt[k] = g[k];

    *p_entry = (uint64_t)(uintptr_t)&ap_entry;

    const uint8_t vector = 0x08;   // 0x8000 >> 12 (satoru)
    const uint32_t STACK_SZ = 16 * 1024;
    int consecutive_fail = 0;

    for (uint32_t i = 1; i < g_cpu_count; i++) {
        void* stk = PMM::AllocBytes(STACK_SZ);
        if (!stk) {
            SerialLogger::Log("[SMP] cpu"); SerialLogger::LogDec((int)i);
            SerialLogger::Log(" no stack, skipped\r\n");
            continue;
        }
        *p_stack = (uint64_t)(uintptr_t)stk + STACK_SZ;   // top, grows down (satoru)
        g_ap_stack_bytes += STACK_SZ;
        g_cpus[i].kernel_stack_top = *p_stack;
        g_cpus[i].online = 0;

        uint32_t apic = g_cpus[i].apic_id;

        // INIT-SIPI-SIPI as linux wakeup_secondary_cpu_via_init: level INIT assert,
        // icr idle, 10ms, level INIT deassert, icr idle, then two STARTUPs (300us
        // then 200us after each, icr idle between). every wait is bounded by a
        // pause-iteration cap as well as the tsc. (satoru)
        // icr_send: msr 0x830 with the full 32-bit id in x2apic mode. (satoru)
        icr_send(apic, 0xC500);             // INIT, level, assert (satoru)
        icr_idle();
        busy_us(10000);
        icr_send(apic, 0x8500);             // INIT, level, deassert (satoru)
        icr_idle();
        for (int s = 0; s < 2; s++) {
            icr_send(apic, 0x4600 | vector);     // STARTUP + trampoline vector (satoru)
            busy_us(300);
            icr_idle();
            busy_us(200);
        }

        // wait for the ap to flip its online flag. hybrid parts can take a good
        // while to come out of a deep c-state: 500ms budget, then give up on this
        // one (it is left offline: never counted, scheduled or waited on by a
        // tlb shootdown). (satoru)
        int tries = 500;
        while (!g_cpus[i].online && tries-- > 0) busy_us(1000);

        SerialLogger::Log("[SMP] cpu"); SerialLogger::LogDec((int)i);
        SerialLogger::Log(g_cpus[i].online ? " online (apic " : " FAILED to start (apic ");
        SerialLogger::LogDec((int)apic);
        SerialLogger::Log(", type ");
        SerialLogger::Log(g_x2apic ? "x2apic" : (apic < 255 ? "lapic" : "x2apic"));
        SerialLogger::Log(")\r\n");
        if (g_cpus[i].online && i < SMP_MAX_CPUS) cr_log(i);   // (satoru)
        if (g_cpus[i].online && i < SMP_MAX_CPUS && g_tscadj_boot[i] != 0) {
            g_tscadj_nonzero++;
            tsc_adjust_log(i, g_tscadj_boot[i]);
        }
        // a dead madt or a dead trampoline fails every ap the same way: stop after
        // a few in a row instead of burning 500ms each. (satoru)
        if (g_cpus[i].online) consecutive_fail = 0;
        else if (++consecutive_fail >= 4) {
            SerialLogger::Log("[SMP] 4 aps in a row failed - not trying the rest\r\n");
            break;
        }
    }

    SerialLogger::Log("[SMP] "); SerialLogger::LogDec((int)OnlineCount());
    SerialLogger::Log("/"); SerialLogger::LogDec((int)g_cpu_count);
    SerialLogger::Log(" cpus online\r\n");
    if (g_tscadj_have) {
        SerialLogger::Log("[tsc] tsc_adjust checked on "); SerialLogger::LogDec((int)OnlineCount());
        SerialLogger::Log(" cpus: "); SerialLogger::LogDec((int)g_tscadj_nonzero);
        SerialLogger::Log(g_tscadj_nonzero == 0 ? " nonzero, rdtsc already agreed\r\n"
                          : (g_tscadj_fix ? " nonzero, all forced to 0 (rdtsc now agrees across cpus)\r\n"
                                          : " nonzero and KEPT, rdtsc differs across cpus (kurono.tscadjust=0)\r\n"));
    }
}

void SMP::TscAdjustBoot(bool fix, bool inject_test) {
    g_tscadj_fix = fix;
    g_tscadj_have = tsc_adjust_supported();
    if (!g_tscadj_have) return;   // older cpu or a hypervisor without the msr: nothing to do, nothing logged (satoru)
    if (inject_test) {
        // kurono.tscadjust.test=1: put the laptop's firmware value on the bsp
        // so the sanitize (or, with kurono.tscadjust=0, the skew itself) can be
        // exercised under kvm. (satoru)
        wrmsr(MSR_IA32_TSC_ADJUST, (uint64_t)(int64_t)-1797404919LL);
        SerialLogger::Log("[tsc] test: bsp tsc_adjust set to the laptop's firmware value (kurono.tscadjust.test=1)\r\n");
    }
    int64_t v = tsc_adjust_sanitize_this_cpu();
    g_tscadj_boot[0] = v;
    if (v != 0) {
        g_tscadj_nonzero++;
        tsc_adjust_log(0, v);
    }
}

void SMP::CrPolicyBsp(bool wp_on, uint64_t cr4_set) {
    g_wp_on = wp_on;
    g_cr4_bsp_set = cr4_set & CR4_POLICY_MASK;
    g_cr0_in[0] = read_cr0();
    g_cr4_in[0] = read_cr4();
    uint64_t cr0 = g_cr0_in[0] | CR0_NE_BIT;
    // flag off: keep the WP bit this cpu came in with (an efi bsp keeps the
    // firmware's WP=1 as before; the default flips only after a laptop test). (satoru)
    if (wp_on) cr0 |= CR0_WP_BIT;
    if (cr0 != g_cr0_in[0]) write_cr0(cr0);
    // the caller writes the bsp's cr4 bits itself, right after this. (satoru)
    g_cr0_out[0] = read_cr0();
    g_cr4_out[0] = g_cr4_in[0] | g_cr4_bsp_set;
    SerialLogger::Log(wp_on ? "[cr] kurono.wp=1: CR0.WP set on every cpu\r\n"
                            : "[cr] CR0.WP left as each cpu booted (kurono.wp=1 sets it everywhere)\r\n");
    cr_log(0);
}

void SMP::ApplyCrPolicyAp() {
    uint32_t ix = CpuIndex();
    if (ix >= SMP_MAX_CPUS) return;
    uint64_t c0 = read_cr0(), c4 = read_cr4();
    g_cr0_in[ix] = c0;
    g_cr4_in[ix] = c4;
    uint64_t n0 = c0 | CR0_NE_BIT;
    if (g_wp_on) n0 |= CR0_WP_BIT;   // flag off: the trampoline's value, as before (satoru)
    if (n0 != c0) write_cr0(n0);
    // only bits this cpu reports itself (a hybrid part's cores agree, but a
    // bit the ap lacks would #GP the cr4 write). (satoru)
    uint32_t a = 0, b = 0, c = 0, d = 0, b7 = 0, c7 = 0;
    cpuid_ex(0, 0, &a, &b, &c, &d);
    if (a >= 7) cpuid_ex(7, 0, &a, &b7, &c7, &d);
    uint64_t want = g_cr4_bsp_set;
    if (!(b7 & (1u << 7))) want &= ~(1ULL << 20);
    if (!(c7 & (1u << 2))) want &= ~(1ULL << 11);
    if ((c4 | want) != c4) write_cr4(c4 | want);
    g_cr0_out[ix] = read_cr0();
    g_cr4_out[ix] = read_cr4();
}

bool SMP::WpOn() { return g_wp_on; }

bool SMP::WpKernelWriteFallback(uint64_t cr2, uint64_t rip, uint64_t pte_flags) {
    if (!(read_cr0() & CR0_WP_BIT)) return false;
    uint32_t ix = CpuIndex();
    if (ix >= SMP_MAX_CPUS) ix = 0;
    uint64_t pg = cr2 & ~0xFFFULL;
    // the live pte already allows the write: this cpu's tlb is stale (a sibling
    // privatised the page). flush and retry, bounded per cpu. (satoru)
    if ((pte_flags & PTE_PRESENT) && (pte_flags & PTE_WRITABLE) && (pte_flags & PTE_USER)) {
        if (g_wp_stale_va[ix] != pg) { g_wp_stale_va[ix] = pg; g_wp_stale_n[ix] = 0; }
        if (g_wp_stale_n[ix]++ < 8) {
            KernelVMM::InvalidatePage(pg);
            return true;
        }
    }
    __atomic_add_fetch(&g_wp_fallback_count, 1, __ATOMIC_RELAXED);
    // one line per site: a retry of the same store on this cpu is not logged again. (satoru)
    bool same_site = (g_wp_fault_rip[ix] == rip && g_wp_last_va[ix] == pg);
    g_wp_fault_rip[ix] = rip;
    g_wp_last_va[ix] = pg;
    if (!same_site && __atomic_add_fetch(&g_wp_fallback_logs, 1, __ATOMIC_RELAXED) <= 32) {
        SerialLogger::Log("[wpro] ring-0 write to a read-only user page va=");
        SerialLogger::LogHex64(cr2);
        SerialLogger::Log(" rip="); SerialLogger::LogHex64(rip);
        SerialLogger::Log(" pte="); SerialLogger::LogHex64(pte_flags);
        SerialLogger::Log((pte_flags & PTE_COW) ? " COW-unresolved" : " ro");
        SerialLogger::Log(" cpu"); SerialLogger::LogDec((int)ix);
        SerialLogger::Log(" (no uaccess fixup: store runs with WP suspended)\r\n");
    }
    if (!g_wp_suspended[ix]) {
        g_wp_suspended[ix] = 1;
        __atomic_add_fetch(&g_wp_suspended_any, 1, __ATOMIC_RELEASE);
    }
    write_cr0(read_cr0() & ~CR0_WP_BIT);
    return true;
}

void SMP::WpRearm(uint64_t rip) {
    if (!__atomic_load_n(&g_wp_suspended_any, __ATOMIC_ACQUIRE)) return;
    uint32_t ix = CpuIndex();
    if (ix >= SMP_MAX_CPUS || !g_wp_suspended[ix]) return;
    // an interrupt taken AT the suspended store (pending when the fault
    // returned, or mid rep-movs) must not re-arm yet, or the store faults again
    // forever under a steady interrupt load. re-arm once this cpu got past it. (satoru)
    if (rip == g_wp_fault_rip[ix]) return;
    g_wp_suspended[ix] = 0;
    __atomic_sub_fetch(&g_wp_suspended_any, 1, __ATOMIC_RELEASE);
    write_cr0(read_cr0() | CR0_WP_BIT);
}

int64_t SMP::TscAdjustFirmware(uint32_t idx) { return idx < SMP_MAX_CPUS ? g_tscadj_boot[idx] : 0; }
bool SMP::TscAdjustKept() { return g_tscadj_have && !g_tscadj_fix && g_tscadj_nonzero != 0; }
// end (satoru)
