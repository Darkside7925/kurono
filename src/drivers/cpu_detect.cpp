#include "cpu_detect.h"
#include "../drivers/serial.h"

//  kurono os  -  cpu detection & feature identification
//  real cpuid-based processor identification

CpuInfo CPUDetect::info;

void CPUDetect::CPUID(uint32_t leaf, uint32_t subleaf,
                      uint32_t* eax, uint32_t* ebx, uint32_t* ecx, uint32_t* edx) {
    asm volatile("cpuid"
                 : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                 : "a"(leaf), "c"(subleaf));
}

//  vendor detection
void CPUDetect::DetectVendor() {
    uint32_t eax, ebx, ecx, edx;
    CPUID(0, 0, &eax, &ebx, &ecx, &edx);

    info.max_cpuid = eax;

    // vendor string is in ebx-edx-ecx order
    *(uint32_t*)&info.vendor_string[0] = ebx;
    *(uint32_t*)&info.vendor_string[4] = edx;
    *(uint32_t*)&info.vendor_string[8] = ecx;
    info.vendor_string[12] = '\0';

    // identify vendor
    info.vendor = CPU_VENDOR_UNKNOWN;

    // compare vendor string manually (no strcmp in freestanding)
    // "genuineintel"
    if (ebx == 0x756E6547 && edx == 0x49656E69 && ecx == 0x6C65746E)
        info.vendor = CPU_VENDOR_INTEL;
    // "authenticamd"
    else if (ebx == 0x68747541 && edx == 0x69746E65 && ecx == 0x444D4163)
        info.vendor = CPU_VENDOR_AMD;
    // "centaurhauls" (via)
    else if (ebx == 0x746E6543 && edx == 0x48727561 && ecx == 0x736C7561)
        info.vendor = CPU_VENDOR_VIA;
    // "hygongenuine"
    else if (ebx == 0x6F677948 && edx == 0x6E65476E && ecx == 0x656E6975)
        info.vendor = CPU_VENDOR_HYGON;

    // extended cpuid max
    CPUID(0x80000000, 0, &eax, &ebx, &ecx, &edx);
    info.max_ext_cpuid = eax;
}

//  model / stepping / family
void CPUDetect::DetectModel() {
    if (info.max_cpuid < 1) return;

    uint32_t eax, ebx, ecx, edx;
    CPUID(1, 0, &eax, &ebx, &ecx, &edx);

    info.stepping   = eax & 0xF;
    info.model      = (eax >> 4) & 0xF;
    info.family     = (eax >> 8) & 0xF;
    info.ext_model  = (eax >> 16) & 0xF;
    info.ext_family = (eax >> 20) & 0xFF;

    // calculate effective family/model
    if (info.family == 0xF)
        info.family += info.ext_family;
    if (info.family == 0x6 || info.family == 0xF)
        info.model += (info.ext_model << 4);

    info.clflush_size = ((ebx >> 8) & 0xFF) * 8;
    info.initial_apic_id = (ebx >> 24) & 0xFF;
}

//  brand string (cpuid 0x80000002 - 0x80000004)
void CPUDetect::DetectBrand() {
    info.brand_string[0] = '\0';

    if (info.max_ext_cpuid < 0x80000004) return;

    uint32_t* ptr = (uint32_t*)info.brand_string;

    for (uint32_t leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
        uint32_t eax, ebx, ecx, edx;
        CPUID(leaf, 0, &eax, &ebx, &ecx, &edx);
        *ptr++ = eax;
        *ptr++ = ebx;
        *ptr++ = ecx;
        *ptr++ = edx;
    }
    info.brand_string[48] = '\0';

    // trim leading spaces
    int start = 0;
    while (info.brand_string[start] == ' ') start++;
    if (start > 0) {
        int i = 0;
        while (info.brand_string[start + i]) {
            info.brand_string[i] = info.brand_string[start + i];
            i++;
        }
        info.brand_string[i] = '\0';
    }
}

//  feature flag detection
void CPUDetect::DetectFeatures() {
    info.features_edx = 0;
    info.features_ecx = 0;
    info.features_ebx7 = 0;
    info.features_amd_edx = 0;

    if (info.max_cpuid >= 1) {
        uint32_t eax, ebx, ecx, edx;
        CPUID(1, 0, &eax, &ebx, &ecx, &edx);
        info.features_edx = edx;
        info.features_ecx = ecx;
    }

    // extended features (leaf 7)
    if (info.max_cpuid >= 7) {
        uint32_t eax, ebx, ecx, edx;
        CPUID(7, 0, &eax, &ebx, &ecx, &edx);
        info.features_ebx7 = ebx;
    }

    // amd/extended features
    if (info.max_ext_cpuid >= 0x80000001) {
        uint32_t eax, ebx, ecx, edx;
        CPUID(0x80000001, 0, &eax, &ebx, &ecx, &edx);
        info.features_amd_edx = edx;
    }
}

//  cache detection
void CPUDetect::DetectCache() {
    info.num_caches = 0;

    // method 1: deterministic cache parameters (cpuid eax=4)
    if (info.vendor == CPU_VENDOR_INTEL && info.max_cpuid >= 4) {
        for (int idx = 0; idx < 8; idx++) {
            uint32_t eax, ebx, ecx, edx;
            CPUID(4, idx, &eax, &ebx, &ecx, &edx);

            int type = eax & 0x1F;
            if (type == 0) break;  // no more caches

            CpuCacheInfo& c = info.cache[info.num_caches];
            c.type = type;
            c.level = (eax >> 5) & 0x7;
            c.line_size = (ebx & 0xFFF) + 1;
            c.associativity = ((ebx >> 22) & 0x3FF) + 1;
            int partitions = ((ebx >> 12) & 0x3FF) + 1;
            c.sets = ecx + 1;
            c.size_kb = (c.associativity * partitions * c.line_size * c.sets) / 1024;

            info.num_caches++;
            if (info.num_caches >= 8) break;
        }
    }
    // method 2: amd cache info (cpuid 0x80000005/6)
    else if (info.vendor == CPU_VENDOR_AMD) {
        // l1 data cache
        if (info.max_ext_cpuid >= 0x80000005) {
            uint32_t eax, ebx, ecx, edx;
            CPUID(0x80000005, 0, &eax, &ebx, &ecx, &edx);

            CpuCacheInfo& l1d = info.cache[info.num_caches++];
            l1d.level = 1;
            l1d.type = 1;  // data
            l1d.size_kb = (ecx >> 24) & 0xFF;
            l1d.line_size = ecx & 0xFF;
            l1d.associativity = (ecx >> 16) & 0xFF;
            l1d.sets = 0;

            CpuCacheInfo& l1i = info.cache[info.num_caches++];
            l1i.level = 1;
            l1i.type = 2;  // instruction
            l1i.size_kb = (edx >> 24) & 0xFF;
            l1i.line_size = edx & 0xFF;
            l1i.associativity = (edx >> 16) & 0xFF;
            l1i.sets = 0;
        }

        // l2/l3
        if (info.max_ext_cpuid >= 0x80000006) {
            uint32_t eax, ebx, ecx, edx;
            CPUID(0x80000006, 0, &eax, &ebx, &ecx, &edx);

            CpuCacheInfo& l2 = info.cache[info.num_caches++];
            l2.level = 2;
            l2.type = 3;  // unified
            l2.size_kb = (ecx >> 16) & 0xFFFF;
            l2.line_size = ecx & 0xFF;
            l2.associativity = (ecx >> 12) & 0xF;
            l2.sets = 0;

            int l3_size = ((edx >> 18) & 0x3FFF) * 512;  // in kb
            if (l3_size > 0) {
                CpuCacheInfo& l3 = info.cache[info.num_caches++];
                l3.level = 3;
                l3.type = 3;
                l3.size_kb = l3_size;
                l3.line_size = edx & 0xFF;
                l3.associativity = (edx >> 12) & 0xF;
                l3.sets = 0;
            }
        }
    }
}

//  topology  -  core / thread counting
void CPUDetect::DetectTopology() {
    info.topology.physical_cores = 1;
    info.topology.logical_cores = 1;
    info.topology.threads_per_core = 1;
    info.topology.sockets = 1;

    if (info.max_cpuid < 1) return;

    uint32_t eax, ebx, ecx, edx;
    CPUID(1, 0, &eax, &ebx, &ecx, &edx);

    // logical processor count from cpuid.1 ebx[23:16]
    int logical_per_package = (ebx >> 16) & 0xFF;
    if (logical_per_package == 0) logical_per_package = 1;

    info.topology.logical_cores = logical_per_package;

    // try leaf 0xb (x2apic topology) for accurate counts
    if (info.max_cpuid >= 0xB) {
        int threads = 0, cores = 0;

        CPUID(0xB, 0, &eax, &ebx, &ecx, &edx);
        if ((ecx >> 8 & 0xFF) == 1)  // smt level
            threads = ebx & 0xFFFF;

        CPUID(0xB, 1, &eax, &ebx, &ecx, &edx);
        if ((ecx >> 8 & 0xFF) == 2)  // core level
            cores = ebx & 0xFFFF;

        if (threads > 0 && cores > 0) {
            info.topology.logical_cores = cores;
            info.topology.threads_per_core = (threads > 0 && cores > 0) ? (threads > 1 ? 2 : 1) : 1;
            info.topology.physical_cores = cores / info.topology.threads_per_core;
            if (info.topology.physical_cores < 1) info.topology.physical_cores = 1;
            return;
        }
    }

    // fallback: use htt flag
    if (info.features_edx & CPU_FEAT_HT) {
        // try cpuid leaf 4 (intel) for core count
        if (info.vendor == CPU_VENDOR_INTEL && info.max_cpuid >= 4) {
            CPUID(4, 0, &eax, &ebx, &ecx, &edx);
            int cores_per_package = ((eax >> 26) & 0x3F) + 1;
            info.topology.physical_cores = cores_per_package;
            info.topology.threads_per_core = logical_per_package / cores_per_package;
            if (info.topology.threads_per_core < 1) info.topology.threads_per_core = 1;
        }
        // amd: use cpuid 0x80000008
        else if (info.vendor == CPU_VENDOR_AMD && info.max_ext_cpuid >= 0x80000008) {
            CPUID(0x80000008, 0, &eax, &ebx, &ecx, &edx);
            int nc = (ecx & 0xFF) + 1;
            info.topology.physical_cores = nc;
            info.topology.threads_per_core = logical_per_package / nc;
            if (info.topology.threads_per_core < 1) info.topology.threads_per_core = 1;
        }
    }

    info.topology.logical_cores = info.topology.physical_cores * info.topology.threads_per_core;
}

//  frequency detection
void CPUDetect::DetectFrequency() {
    info.frequency.tsc_frequency = 0;
    info.frequency.base_mhz = 0;
    info.frequency.max_mhz = 0;

    // method 1: cpuid leaf 0x15 (tsc/core crystal clock)
    if (info.vendor == CPU_VENDOR_INTEL && info.max_cpuid >= 0x15) {
        uint32_t eax, ebx, ecx, edx;
        CPUID(0x15, 0, &eax, &ebx, &ecx, &edx);

        if (eax != 0 && ebx != 0 && ecx != 0) {
            // tsc freq = crystal * ebx / eax
            info.frequency.tsc_frequency = ((uint64_t)ecx * ebx) / eax;
            info.frequency.base_mhz = (int)(info.frequency.tsc_frequency / 1000000);
        }
    }

    // method 2: cpuid leaf 0x16 (processor frequency info)
    if (info.vendor == CPU_VENDOR_INTEL && info.max_cpuid >= 0x16) {
        uint32_t eax, ebx, ecx, edx;
        CPUID(0x16, 0, &eax, &ebx, &ecx, &edx);

        if (eax & 0xFFFF) info.frequency.base_mhz = eax & 0xFFFF;
        if (ebx & 0xFFFF) info.frequency.max_mhz = ebx & 0xFFFF;
    }

    // method 3: tsc calibration via pit (programmable interval timer)
    if (info.frequency.base_mhz == 0 && (info.features_edx & CPU_FEAT_TSC)) {
        // program pit channel 2 for one-shot, 10ms
        // pit frequency = 1193182 hz
        // 10ms = 11932 ticks

        uint16_t pit_count = 11932;

        // gate pit channel 2
        uint8_t gate = 0;
        asm volatile("inb $0x61, %0" : "=a"(gate));
        gate &= 0xFC;  // disable speaker, gate off
        gate |= 0x01;  // gate on for channel 2
        asm volatile("outb %0, $0x61" : : "a"(gate));

        // program channel 2: mode 0 (interrupt on terminal count)
        asm volatile("outb %0, $0x43" : : "a"((uint8_t)0xB0));  // ch2, lobyte/hibyte, mode 0
        asm volatile("outb %0, $0x42" : : "a"((uint8_t)(pit_count & 0xFF)));
        asm volatile("outb %0, $0x42" : : "a"((uint8_t)(pit_count >> 8)));

        // read tsc start
        uint32_t lo1, hi1;
        asm volatile("rdtsc" : "=a"(lo1), "=d"(hi1));

        // wait for pit to count down (bit 5 of port 0x61 goes high)
        for (;;) {
            uint8_t status = 0;
            asm volatile("inb $0x61, %0" : "=a"(status));
            if (status & 0x20) break;
        }

        // read tsc end
        uint32_t lo2, hi2;
        asm volatile("rdtsc" : "=a"(lo2), "=d"(hi2));

        uint64_t tsc_start = ((uint64_t)hi1 << 32) | lo1;
        uint64_t tsc_end   = ((uint64_t)hi2 << 32) | lo2;
        uint64_t tsc_delta = tsc_end - tsc_start;

        // tsc_delta / 10ms = ticks per second / 100
        info.frequency.tsc_frequency = tsc_delta * 100;
        info.frequency.base_mhz = (int)(info.frequency.tsc_frequency / 1000000);
    }
}

//  init
bool CPUDetect::Init() {
    // zero out
    for (int i = 0; i < (int)sizeof(info); i++)
        ((uint8_t*)&info)[i] = 0;

    // check cpuid support (flip id bit in eflags)
    uint64_t flags_before, flags_after;
    asm volatile(
        "pushfq\n"
        "pop %0\n"
        "mov %0, %1\n"
        "xor $0x200000, %1\n"
        "push %1\n"
        "popfq\n"
        "pushfq\n"
        "pop %1\n"
        : "=r"(flags_before), "=r"(flags_after)
    );

    if (flags_before == flags_after) {
        SerialLogger::Log("[CPU] CPUID not supported!\r\n");
        return false;
    }

    DetectVendor();
    DetectModel();
    DetectBrand();
    DetectFeatures();
    DetectCache();
    DetectTopology();
    DetectFrequency();

    SerialLogger::Log("[CPU] ");
    SerialLogger::Log(info.brand_string[0] ? info.brand_string : info.vendor_string);
    SerialLogger::Log("\r\n");
    SerialLogger::Log("[CPU] Family=");
    SerialLogger::LogDec(info.family);
    SerialLogger::Log(" Model=");
    SerialLogger::LogDec(info.model);
    SerialLogger::Log(" Step=");
    SerialLogger::LogDec(info.stepping);
    SerialLogger::Log("\r\n");
    SerialLogger::Log("[CPU] Cores=");
    SerialLogger::LogDec(info.topology.physical_cores);
    SerialLogger::Log(" Threads=");
    SerialLogger::LogDec(info.topology.logical_cores);
    SerialLogger::Log(" @ ");
    SerialLogger::LogDec(info.frequency.base_mhz);
    SerialLogger::Log(" MHz\r\n");

    return true;
}

//  getters & feature queries
CpuInfo CPUDetect::GetInfo() { return info; }

bool CPUDetect::HasSSE()    { return info.features_edx & CPU_FEAT_SSE; }
bool CPUDetect::HasSSE2()   { return info.features_edx & CPU_FEAT_SSE2; }
bool CPUDetect::HasSSE3()   { return info.features_ecx & CPU_EXT_SSE3; }
bool CPUDetect::HasSSSE3()  { return info.features_ecx & CPU_EXT_SSSE3; }
bool CPUDetect::HasSSE41()  { return info.features_ecx & CPU_EXT_SSE41; }
bool CPUDetect::HasSSE42()  { return info.features_ecx & CPU_EXT_SSE42; }
bool CPUDetect::HasAVX()    { return info.features_ecx & CPU_EXT_AVX; }
bool CPUDetect::HasAVX2()   { return info.features_ebx7 & CPU_EXT7_AVX2; }
bool CPUDetect::HasAVX512() { return info.features_ebx7 & CPU_EXT7_AVX512F; }
bool CPUDetect::HasAES()    { return info.features_ecx & CPU_EXT_AES; }
bool CPUDetect::HasFMA()    { return info.features_ecx & CPU_EXT_FMA; }
bool CPUDetect::HasMMX()    { return info.features_edx & CPU_FEAT_MMX; }
bool CPUDetect::HasRDRAND() { return info.features_ecx & CPU_EXT_RDRAND; }
bool CPUDetect::HasRDSEED() { return info.features_ebx7 & CPU_EXT7_RDSEED; }
bool CPUDetect::HasTSC()    { return info.features_edx & CPU_FEAT_TSC; }
bool CPUDetect::HasMSR()    { return info.features_edx & CPU_FEAT_MSR; }
bool CPUDetect::HasAPIC()   { return info.features_edx & CPU_FEAT_APIC; }
bool CPUDetect::HasX2APIC() { return info.features_ecx & CPU_EXT_X2APIC; }
bool CPUDetect::HasNX()     { return info.features_amd_edx & CPU_AMD_NX; }
bool CPUDetect::HasLongMode() { return info.features_amd_edx & CPU_AMD_LM; }
bool CPUDetect::Has1GBPages() { return info.features_amd_edx & CPU_AMD_PAGE1GB; }
bool CPUDetect::HasHyperThreading() { return info.features_edx & CPU_FEAT_HT; }
bool CPUDetect::HasXSAVE()  { return info.features_ecx & CPU_EXT_XSAVE; }

const char* CPUDetect::GetVendorName() {
    switch (info.vendor) {
        case CPU_VENDOR_INTEL: return "Intel";
        case CPU_VENDOR_AMD:   return "AMD";
        case CPU_VENDOR_VIA:   return "VIA";
        case CPU_VENDOR_HYGON: return "Hygon";
        default:               return info.vendor_string;
    }
}

const char* CPUDetect::GetBrandString() {
    if (info.brand_string[0]) return info.brand_string;
    return "Unknown CPU";
}

int CPUDetect::GetCoreCount()  { return info.topology.physical_cores; }
int CPUDetect::GetThreadCount(){ return info.topology.logical_cores; }
int CPUDetect::GetBaseMHz()    { return info.frequency.base_mhz; }

//  print full cpu information
void CPUDetect::PrintInfo() {
    SerialLogger::Log("=== CPU Information ===\r\n");
    SerialLogger::Log("Vendor: ");
    SerialLogger::Log(GetVendorName());
    SerialLogger::Log(" (");
    SerialLogger::Log(info.vendor_string);
    SerialLogger::Log(")\r\n");
    SerialLogger::Log("Brand: ");
    SerialLogger::Log(GetBrandString());
    SerialLogger::Log("\r\n");
    SerialLogger::Log("Family: ");
    SerialLogger::LogDec(info.family);
    SerialLogger::Log("  Model: ");
    SerialLogger::LogDec(info.model);
    SerialLogger::Log("  Stepping: ");
    SerialLogger::LogDec(info.stepping);
    SerialLogger::Log("\r\n");
    SerialLogger::Log("Cores: ");
    SerialLogger::LogDec(info.topology.physical_cores);
    SerialLogger::Log(" physical, ");
    SerialLogger::LogDec(info.topology.logical_cores);
    SerialLogger::Log(" logical (");
    SerialLogger::LogDec(info.topology.threads_per_core);
    SerialLogger::Log(" threads/core)\r\n");
    SerialLogger::Log("Frequency: ");
    SerialLogger::LogDec(info.frequency.base_mhz);
    SerialLogger::Log(" MHz base");
    if (info.frequency.max_mhz > 0) {
        SerialLogger::Log(", ");
        SerialLogger::LogDec(info.frequency.max_mhz);
        SerialLogger::Log(" MHz max turbo");
    }
    SerialLogger::Log("\r\n");

    // cache info
    for (int i = 0; i < info.num_caches; i++) {
        const char* tname = "Unknown";
        if (info.cache[i].type == 1) tname = "Data";
        else if (info.cache[i].type == 2) tname = "Instruction";
        else if (info.cache[i].type == 3) tname = "Unified";

        SerialLogger::Log("L");
        SerialLogger::LogDec(info.cache[i].level);
        SerialLogger::Log(" ");
        SerialLogger::Log(tname);
        SerialLogger::Log(" Cache: ");
        SerialLogger::LogDec(info.cache[i].size_kb);
        SerialLogger::Log(" KB, ");
        SerialLogger::LogDec(info.cache[i].associativity);
        SerialLogger::Log("-way, ");
        SerialLogger::LogDec(info.cache[i].line_size);
        SerialLogger::Log("-byte line\r\n");
    }

    // feature flags
    SerialLogger::Log("Features:");
    if (HasMMX())     SerialLogger::Log(" MMX");
    if (HasSSE())     SerialLogger::Log(" SSE");
    if (HasSSE2())    SerialLogger::Log(" SSE2");
    if (HasSSE3())    SerialLogger::Log(" SSE3");
    if (HasSSSE3())   SerialLogger::Log(" SSSE3");
    if (HasSSE41())   SerialLogger::Log(" SSE4.1");
    if (HasSSE42())   SerialLogger::Log(" SSE4.2");
    if (HasAVX())     SerialLogger::Log(" AVX");
    if (HasAVX2())    SerialLogger::Log(" AVX2");
    if (HasAVX512())  SerialLogger::Log(" AVX-512");
    if (HasAES())     SerialLogger::Log(" AES-NI");
    if (HasFMA())     SerialLogger::Log(" FMA3");
    if (HasRDRAND())  SerialLogger::Log(" RDRAND");
    if (HasRDSEED())  SerialLogger::Log(" RDSEED");
    if (HasXSAVE())   SerialLogger::Log(" XSAVE");
    if (HasNX())      SerialLogger::Log(" NX");
    if (HasLongMode())SerialLogger::Log(" x86-64");
    SerialLogger::Log("\r\n");
}
