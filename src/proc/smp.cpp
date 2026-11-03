#include "smp.h"
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
                    if (id >= 255) { big++; g_cand_skipped++; }   // xapic cannot address it (satoru)
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
            SerialLogger::Log("[SMP] !!! x2apic is LOCKED on (legacy xapic disabled by hardware): "
                              "driving the lapic through msrs, single cpu only\r\n");
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
    else if (BootAcpi::IsEfiBoot()) { cap = 8; cap_default = true; }
    if (cap > SMP_MAX_CPUS) cap = SMP_MAX_CPUS;
    if (cap < 1) cap = 1;
    if (g_x2apic || g_lapic_dead) cap = 1;   // aps cannot be brought up without a usable xapic path (satoru)

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
    uint32_t id = ApicId();
    if (id < 256 && g_apicid_to_index[id] >= 0) return (uint32_t)g_apicid_to_index[id];
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
    // match the bsp's mode before the first lapic mmio access: an ap that comes out of
    // INIT-SIPI with x2apic still on (firmware left it) would hit the dead window. no
    // serial here (shared console); the bsp reports the outcome via the online flag. (satoru)
    (void)x2apic_to_xapic();
    SMP::LapicEnable();
    HAL::SetupAPCpuState();  // own gdt+tss, shared idt, per-core syscall msrs (satoru)
    SMP::SetupGsBase();      // KERNEL_GS_BASE -> this ap's PerCpu (after the gs reload) (satoru)
    PerCpu* me = SMP::Current();
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
        {
            uint64_t nowm = Timer::GetRealMs64();
            if (nowm != last_maint_ms) {   // at most once per ms (satoru)
                last_maint_ms = nowm;
                Scheduler::ApIdleMaint();
            }
        }
        if (g_ap_thread_sched) {
            Process* t = Scheduler::ClaimReadyThreadForCpu(idx);
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
                if (Scheduler::LoadUserFrame(t, &f)) {
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
    f->rflags = 0x202ULL;
}

//  tlb shootdown. sender: pulse vector 0x41 at every other cpu (all-excluding-
//  self shorthand) and wait - bounded - for each online peer to ack. bounded
//  because a peer may briefly sit with IF=0 (a spinlock hold); the per-ap timer
//  cr3 reload in ApTimerPreempt bounds any missed window to one tick. (satoru)
void SMP::BroadcastTlbFlush() {
    uint32_t online = OnlineCount();
    if (online <= 1) return;
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
    LapicWrite(0x300, (3u << 18) | (1u << 14) | 0x41u);
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
    if (c3 == k) return;
    __asm__ volatile("mov %0, %%cr3" : : "r"(k) : "memory");
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
    *p_cr3 = cr3;

    struct __attribute__((packed)) { uint16_t limit; uint64_t base; } gdtr;
    __asm__ volatile("sgdt %0" : "=m"(gdtr));
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
        g_cpus[i].kernel_stack_top = *p_stack;
        g_cpus[i].online = 0;

        uint32_t apic = g_cpus[i].apic_id;

        // INIT-SIPI-SIPI as linux wakeup_secondary_cpu_via_init: level INIT assert,
        // icr idle, 10ms, level INIT deassert, icr idle, then two STARTUPs (300us
        // then 200us after each, icr idle between). every wait is bounded by a
        // pause-iteration cap as well as the tsc. (satoru)
        LapicWrite(0x310, apic << 24);
        LapicWrite(0x300, 0xC500);          // INIT, level, assert (satoru)
        icr_idle();
        busy_us(10000);
        LapicWrite(0x310, apic << 24);
        LapicWrite(0x300, 0x8500);          // INIT, level, deassert (satoru)
        icr_idle();
        for (int s = 0; s < 2; s++) {
            LapicWrite(0x310, apic << 24);
            LapicWrite(0x300, 0x4600 | vector);  // STARTUP + trampoline vector (satoru)
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
        SerialLogger::Log(apic < 255 ? "lapic" : "x2apic");
        SerialLogger::Log(")\r\n");
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
}
// end (satoru)
