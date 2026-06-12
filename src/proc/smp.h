#pragma once
#include "../kernel/types.h"

//  kurono smp  -  multi-core support. phase 1 is foundation only: enable the local
//  apic, enumerate the cpus (acpi madt, with a cpuid/sequential fallback), and
//  build the per-cpu data blocks. starting the application processors (sipi) and
//  per-cpu scheduling come in later phases. (satoru)

#define SMP_MAX_CPUS 32

struct Process;  // fwd  -  set per-cpu in the scheduler phase (satoru)

//  one of these per logical cpu, indexed by a dense cpu index (0 = bsp). the
//  apic-id -> index map lets a running cpu find its own block in O(1). (satoru)
struct PerCpu {
    // THESE TWO MUST STAY AT OFFSETS 0 AND 8  -  the SYSCALL entry stub reads them
    // gs-relative (gs base = &this PerCpu, set via swapgs) so that two cores can
    // syscall at once without sharing one global kernel stack. (satoru)
    uint64_t user_rsp_save;      // offset 0: scratch for the user rsp at entry (satoru)
    uint64_t kernel_rsp;         // offset 8: this cpu's syscall kernel stack (satoru)
    uint32_t cpu_index;          // dense index, 0 = bsp (satoru)
    uint32_t apic_id;            // hardware local apic id (satoru)
    volatile uint32_t online;    // 0 until the cpu marks itself running (satoru)
    uint64_t kernel_stack_top;   // this cpu's idle/bring-up stack (satoru)
    void*    arch;               // per-cpu gdt+tss block (later) (satoru)
    Process* current;            // process currently on this cpu (satoru)
};

static_assert(__builtin_offsetof(PerCpu, user_rsp_save) == 0,
              "syscall stub reads the user-rsp scratch at gs:0 (satoru)");
static_assert(__builtin_offsetof(PerCpu, kernel_rsp) == 8,
              "syscall stub reads the kernel rsp at gs:8 (satoru)");

class SMP {
public:
    // bsp-side: enable the lapic, parse topology, register the bsp as cpu 0. safe
    // to call even on a single-core machine (it just registers the bsp). (satoru)
    static void Init();

    static uint32_t CpuCount();        // logical cpus discovered (satoru)
    static uint32_t OnlineCount();     // cpus that have marked themselves online (satoru)

    static uint32_t ApicId();          // calling cpu's local apic id (reads the lapic) (satoru)
    static uint32_t CpuIndex();        // calling cpu's dense index (via the apic-id map) (satoru)
    static PerCpu*  Current();         // per-cpu block for the calling cpu (satoru)
    static PerCpu*  ByIndex(uint32_t i);

    // point this cpu's KERNEL_GS_BASE at its PerCpu block so the SYSCALL stub can
    // reach gs:0 / gs:8 after swapgs. called by the bsp + every ap. (satoru)
    static void SetupGsBase();

    // xapic register access  -  the lapic mmio window is identity-mapped. (satoru)
    static uint32_t LapicRead(uint32_t reg);
    static void     LapicWrite(uint32_t reg, uint32_t val);
    static void     LapicEnable();     // spurious-int vector reg: set enable + vector (satoru)

    // phase 2  -  bring up the application processors. no-op until implemented. (satoru)
    static void StartAPs();
};
// end (satoru)
