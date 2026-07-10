#include "cpufreq.h"
#include "../drivers/serial.h"

namespace {
    CPUFreq::CPUInfo g_cpus[CPUFreq::CPUFREQ_MAX_CPUS];
    int g_cpu_count = 0;

    inline uint64_t rdmsr(uint32_t msr) {
        uint32_t lo, hi;
        __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
        return ((uint64_t)hi << 32) | lo;
    }
    inline void wrmsr(uint32_t msr, uint64_t val) {
        uint32_t lo = (uint32_t)val;
        uint32_t hi = (uint32_t)(val >> 32);
        __asm__ __volatile__("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
    }
    inline void cpuid(uint32_t leaf, uint32_t subleaf,
                      uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
        __asm__ __volatile__("cpuid"
            : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
            : "a"(leaf), "c"(subleaf));
    }

    constexpr uint32_t MSR_PLATFORM_INFO = 0xCE;
    constexpr uint32_t MSR_IA32_PERF_CTL = 0x199;
    constexpr uint32_t MSR_IA32_PERF_STATUS = 0x198;

    void detect_pstates_intel(CPUFreq::CPUInfo& info) {
        // PLATFORM_INFO[15:8] = max non-turbo ratio
        // PLATFORM_INFO[47:40] = max efficiency ratio
        uint64_t pi = rdmsr(MSR_PLATFORM_INFO);
        uint8_t max_nontb_ratio = (uint8_t)((pi >> 8) & 0xFF);
        uint8_t min_ratio       = (uint8_t)((pi >> 40) & 0xFF);
        if (!max_nontb_ratio) max_nontb_ratio = 20;     // 2.0 GHz fallback
        if (!min_ratio)       min_ratio = 8;             // 800 MHz floor

        info.base_mhz  = (uint16_t)max_nontb_ratio * 100;
        info.turbo_mhz = info.base_mhz;        // refined via TURBO_RATIO_LIMIT later
        info.cur_mhz   = info.base_mhz;

        // Build a uniform ladder from min_ratio..max_nontb_ratio.
        int steps = max_nontb_ratio - min_ratio + 1;
        if (steps > CPUFreq::CPUFREQ_MAX_PSTATES) steps = CPUFreq::CPUFREQ_MAX_PSTATES;
        info.pstate_count = (uint8_t)steps;
        for (int i = 0; i < steps; i++) {
            uint8_t ratio = (uint8_t)(max_nontb_ratio - i);
            info.pstates[i].freq_mhz   = (uint16_t)ratio * 100;
            info.pstates[i].voltage_mv = (uint16_t)(800 + ratio * 5);    // approx
            info.pstates[i].perf_ctl   = ratio;
        }
        info.cur_pstate = 0;
    }
}

namespace CPUFreq {

void Init() {
    for (int i = 0; i < CPUFREQ_MAX_CPUS; i++) g_cpus[i] = {};

    uint32_t a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);
    bool intel = (b == 0x756e6547 && d == 0x49656e69 && c == 0x6c65746e);
    bool amd   = (b == 0x68747541 && d == 0x69746e65 && c == 0x444d4163);

    g_cpu_count = 1;       // bring up BSP first; AP discovery later
    CPUInfo& bsp = g_cpus[0];
    bsp.present  = true;
    bsp.vendor   = intel ? 0 : amd ? 1 : 2;
    bsp.governor = GOV_ONDEMAND;
    bsp.recent_load_pct = 0;

    if (intel) {
        detect_pstates_intel(bsp);
        SerialLogger::Log("CPUFreq: Intel BSP, base=");
        SerialLogger::LogDec(bsp.base_mhz);
        SerialLogger::Log(" MHz, ");
        SerialLogger::LogDec(bsp.pstate_count);
        SerialLogger::Log(" P-states\r\n");
    } else if (amd) {
        // AMD P-state MSRs 0xC0010064..C001006F encode CpuDid/CpuFid/CpuVid.
        bsp.base_mhz  = 2400;
        bsp.turbo_mhz = 3800;
        bsp.cur_mhz   = 2400;
        bsp.pstate_count = 4;
        for (int i = 0; i < 4; i++) {
            bsp.pstates[i].freq_mhz   = (uint16_t)(2400 - i * 400);
            bsp.pstates[i].voltage_mv = (uint16_t)(900 + (3 - i) * 50);
            bsp.pstates[i].perf_ctl   = (uint8_t)i;
        }
        SerialLogger::Log("CPUFreq: AMD BSP, 4 P-states (defaults)\r\n");
    } else {
        bsp.base_mhz  = 2000;
        bsp.cur_mhz   = 2000;
        bsp.pstate_count = 1;
        bsp.pstates[0].freq_mhz = 2000;
        bsp.pstates[0].perf_ctl = 0;
        SerialLogger::Log("CPUFreq: unknown vendor, single fixed P-state\r\n");
    }
}

void Tick(uint32_t cpu_id, bool was_idle) {
    if (cpu_id >= (uint32_t)g_cpu_count) return;
    CPUInfo& c = g_cpus[cpu_id];
    c.total_ticks++;
    if (!was_idle) c.busy_ticks++;

    // Sliding load every 16 ticks.
    if ((c.total_ticks & 0xF) == 0) {
        uint64_t denom = (c.total_ticks > 0) ? c.total_ticks : 1;
        c.recent_load_pct = (uint8_t)((c.busy_ticks * 100) / denom);
        c.busy_ticks = 0;
        c.total_ticks = 0;

        if (c.governor == GOV_ONDEMAND || c.governor == GOV_SCHEDUTIL) {
            // Bump up at >80%, step down at <20%.
            if (c.recent_load_pct > 80 && c.cur_pstate > 0) {
                SetPState(cpu_id, (uint8_t)(c.cur_pstate - 1));
            } else if (c.recent_load_pct < 20 && c.cur_pstate < c.pstate_count - 1) {
                SetPState(cpu_id, (uint8_t)(c.cur_pstate + 1));
            }
        }
    }
}

bool SetGovernor(uint32_t cpu_id, Governor g) {
    if (cpu_id >= (uint32_t)g_cpu_count) return false;
    g_cpus[cpu_id].governor = g;
    if (g == GOV_PERFORMANCE) SetPState(cpu_id, 0);
    if (g == GOV_POWERSAVE)   SetPState(cpu_id, (uint8_t)(g_cpus[cpu_id].pstate_count - 1));
    return true;
}

Governor GetGovernor(uint32_t cpu_id) {
    if (cpu_id >= (uint32_t)g_cpu_count) return GOV_PERFORMANCE;
    return g_cpus[cpu_id].governor;
}

bool SetPState(uint32_t cpu_id, uint8_t pstate_idx) {
    if (cpu_id >= (uint32_t)g_cpu_count) return false;
    CPUInfo& c = g_cpus[cpu_id];
    if (pstate_idx >= c.pstate_count) return false;
    c.cur_pstate = pstate_idx;
    c.cur_mhz    = c.pstates[pstate_idx].freq_mhz;
    if (c.vendor == 0) {        // Intel
        // PERF_CTL[15:8] = ratio
        wrmsr(MSR_IA32_PERF_CTL, ((uint64_t)c.pstates[pstate_idx].perf_ctl) << 8);
    }
    return true;
}

void EnterIdle(uint64_t predicted_residency_us) {
    // Pick depth: <50us → C1, <500us → C2, otherwise C3.
    if (predicted_residency_us < 50) {
        // C1 = HLT
        __asm__ __volatile__("sti; hlt; cli");
    } else if (predicted_residency_us < 500) {
        // MWAIT for C2 if supported.  Hint = 0x10.
        uint32_t a, b, c, d;
        cpuid(5, 0, &a, &b, &c, &d);
        if (c & 0x1) {           // MWAIT supported
            __asm__ __volatile__("monitor" : : "a"(0), "c"(0), "d"(0));
            __asm__ __volatile__("mwait"   : : "a"(0x10), "c"(0));
        } else {
            __asm__ __volatile__("sti; hlt; cli");
        }
    } else {
        // Deep idle - for now also MWAIT C3 hint 0x20.
        __asm__ __volatile__("monitor" : : "a"(0), "c"(0), "d"(0));
        __asm__ __volatile__("mwait"   : : "a"(0x20), "c"(0));
    }
}

bool SuspendToRam() {
    // ACPI S3 entry would: flush dirty cache, save processor state,
    // store wakeup vector at FACS+0x0C, write SLP_TYP|SLP_EN to PM1a/b.
    // We have no real ACPI parser yet, so log + return false.
    SerialLogger::Log("CPUFreq: SuspendToRam (S3) requested - ACPI tables required\r\n");
    return false;
}

bool Hibernate() {
    SerialLogger::Log("CPUFreq: Hibernate (S4) requested - swap image required\r\n");
    return false;
}

const CPUInfo* GetCPU(uint32_t cpu_id) {
    if (cpu_id >= (uint32_t)g_cpu_count) return nullptr;
    return &g_cpus[cpu_id];
}

int CPUCount() { return g_cpu_count; }

}  // namespace CPUFreq
