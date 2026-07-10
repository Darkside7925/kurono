#pragma once
#include <stdint.h>

//  kurono os - cpu detection & feature identification driver
//  uses cpuid instruction for real hardware feature detection

// feature flags (edx from cpuid eax=1)
#define CPU_FEAT_FPU        (1 << 0)
#define CPU_FEAT_VME        (1 << 1)
#define CPU_FEAT_DE         (1 << 2)
#define CPU_FEAT_PSE        (1 << 3)
#define CPU_FEAT_TSC        (1 << 4)
#define CPU_FEAT_MSR        (1 << 5)
#define CPU_FEAT_PAE        (1 << 6)
#define CPU_FEAT_MCE        (1 << 7)
#define CPU_FEAT_CX8        (1 << 8)
#define CPU_FEAT_APIC       (1 << 9)
#define CPU_FEAT_SEP        (1 << 11)
#define CPU_FEAT_MTRR       (1 << 12)
#define CPU_FEAT_PGE        (1 << 13)
#define CPU_FEAT_MCA        (1 << 14)
#define CPU_FEAT_CMOV       (1 << 15)
#define CPU_FEAT_PAT        (1 << 16)
#define CPU_FEAT_PSE36      (1 << 17)
#define CPU_FEAT_PSN        (1 << 18)
#define CPU_FEAT_CLFLUSH    (1 << 19)
#define CPU_FEAT_MMX        (1 << 23)
#define CPU_FEAT_FXSR       (1 << 24)
#define CPU_FEAT_SSE        (1 << 25)
#define CPU_FEAT_SSE2       (1 << 26)
#define CPU_FEAT_HT         (1 << 28)

// extended feature flags (ecx from cpuid eax=1)
#define CPU_EXT_SSE3        (1 << 0)
#define CPU_EXT_PCLMUL      (1 << 1)
#define CPU_EXT_SSSE3       (1 << 9)
#define CPU_EXT_FMA         (1 << 12)
#define CPU_EXT_CX16        (1 << 13)
#define CPU_EXT_SSE41       (1 << 19)
#define CPU_EXT_SSE42       (1 << 20)
#define CPU_EXT_X2APIC      (1 << 21)
#define CPU_EXT_MOVBE       (1 << 22)
#define CPU_EXT_POPCNT      (1 << 23)
#define CPU_EXT_AES         (1 << 25)
#define CPU_EXT_XSAVE       (1 << 26)
#define CPU_EXT_OSXSAVE     (1 << 27)
#define CPU_EXT_AVX         (1 << 28)
#define CPU_EXT_F16C        (1 << 29)
#define CPU_EXT_RDRAND      (1 << 30)

// extended features (ebx from cpuid eax=7, ecx=0)
#define CPU_EXT7_BMI1       (1 << 3)
#define CPU_EXT7_AVX2       (1 << 5)
#define CPU_EXT7_BMI2       (1 << 8)
#define CPU_EXT7_AVX512F    (1 << 16)
#define CPU_EXT7_AVX512DQ   (1 << 17)
#define CPU_EXT7_RDSEED     (1 << 18)
#define CPU_EXT7_AVX512CD   (1 << 28)
#define CPU_EXT7_AVX512BW   (1 << 30)
#define CPU_EXT7_AVX512VL   (1u << 31)

// extended amd features (edx from cpuid eax=0x80000001)
#define CPU_AMD_NX          (1 << 20)
#define CPU_AMD_MMXEXT      (1 << 22)
#define CPU_AMD_FFXSR       (1 << 25)
#define CPU_AMD_PAGE1GB     (1 << 26)
#define CPU_AMD_RDTSCP      (1 << 27)
#define CPU_AMD_LM          (1 << 29)
#define CPU_AMD_3DNOWEXT    (1 << 30)
#define CPU_AMD_3DNOW       (1u << 31)

// cpu vendors
enum CpuVendor {
    CPU_VENDOR_UNKNOWN,
    CPU_VENDOR_INTEL,
    CPU_VENDOR_AMD,
    CPU_VENDOR_VIA,
    CPU_VENDOR_CYRIX,
    CPU_VENDOR_TRANSMETA,
    CPU_VENDOR_HYGON,
    CPU_VENDOR_ZHAOXIN
};

// cache level info
struct CpuCacheInfo {
    int level;         // l1, l2, l3
    int type;          // 1=data, 2=instruction, 3=unified
    int size_kb;       // size in kb
    int line_size;     // cache line size in bytes
    int associativity; // n-way set associative
    int sets;          // number of sets
};

struct CpuTopology {
    int physical_cores;
    int logical_cores;
    int threads_per_core;
    int sockets;
};

struct CpuFrequency {
    uint64_t tsc_frequency;    // tsc ticks per second
    int      base_mhz;        // base clock mhz
    int      max_mhz;         // max turbo mhz (if detectable)
};

// full cpu info
struct CpuInfo {
    // vendor
    CpuVendor vendor;
    char vendor_string[16];    // e.g., "genuineintel"

    // model
    int family;
    int model;
    int stepping;
    int ext_family;
    int ext_model;
    char brand_string[52];     // e.g., "intel(r) core(tm) i7-12700k"

    // features - standard
    uint32_t features_edx;     // cpuid eax=1 edx
    uint32_t features_ecx;     // cpuid eax=1 ecx
    uint32_t features_ebx7;    // cpuid eax=7 ebx
    uint32_t features_amd_edx; // cpuid eax=0x80000001 edx

    // cache
    CpuCacheInfo cache[8];
    int num_caches;

    // topology
    CpuTopology topology;

    // frequency
    CpuFrequency frequency;

    // clflush line size
    int clflush_size;

    // apic id
    int initial_apic_id;

    // max cpuid level
    uint32_t max_cpuid;
    uint32_t max_ext_cpuid;
};

class CPUDetect {
public:
    static bool Init();
    static CpuInfo GetInfo();

    // feature checks
    static bool HasSSE();
    static bool HasSSE2();
    static bool HasSSE3();
    static bool HasSSSE3();
    static bool HasSSE41();
    static bool HasSSE42();
    static bool HasAVX();
    static bool HasAVX2();
    static bool HasAVX512();
    static bool HasAES();
    static bool HasFMA();
    static bool HasMMX();
    static bool HasRDRAND();
    static bool HasRDSEED();
    static bool HasTSC();
    static bool HasMSR();
    static bool HasAPIC();
    static bool HasX2APIC();
    static bool HasNX();
    static bool HasLongMode();
    static bool Has1GBPages();
    static bool HasHyperThreading();
    static bool HasXSAVE();

    // summary
    static const char* GetVendorName();
    static const char* GetBrandString();
    static int GetCoreCount();
    static int GetThreadCount();
    static int GetBaseMHz();

    // detailed dump
    static void PrintInfo();

private:
    static CpuInfo info;

    // raw cpuid wrapper
    static void CPUID(uint32_t leaf, uint32_t subleaf,
                      uint32_t* eax, uint32_t* ebx, uint32_t* ecx, uint32_t* edx);

    static void DetectVendor();
    static void DetectModel();
    static void DetectBrand();
    static void DetectFeatures();
    static void DetectCache();
    static void DetectTopology();
    static void DetectFrequency();
};
