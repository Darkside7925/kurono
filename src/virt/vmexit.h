#pragma once
// ═══════════════════════════════════════════════════════════════════════════
//  Kurono OS — VM Exit / VM Entry Handler
//  Dispatches all VM exits: CPUID, MSR, I/O, interrupts, HLT, EPT faults
// ═══════════════════════════════════════════════════════════════════════════
#include "vmm.h"
#include "ept.h"

// ═══════════════════════════════════════════════════════════════════════════
//  Exit handler result codes
// ═══════════════════════════════════════════════════════════════════════════
enum VMExitAction {
    VMEXIT_CONTINUE   = 0,  // Re-enter guest (advance RIP if needed)
    VMEXIT_HANDLED    = 1,  // Handled, re-enter guest (RIP already advanced)
    VMEXIT_SHUTDOWN   = 2,  // Shut down the VM
    VMEXIT_FATAL      = 3,  // Unrecoverable error
    VMEXIT_REBOOT     = 4   // VM wants to reboot
};

// ═══════════════════════════════════════════════════════════════════════════
//  CPUID leaf override — lets us mask/modify CPUID results for the guest
// ═══════════════════════════════════════════════════════════════════════════
struct CPUIDOverride {
    uint32_t leaf;
    uint32_t subleaf;
    uint32_t eax, ebx, ecx, edx;
    bool     active;
};

#define MAX_CPUID_OVERRIDES 16

// ═══════════════════════════════════════════════════════════════════════════
//  MSR access record — for logging/auditing all MSR reads/writes
// ═══════════════════════════════════════════════════════════════════════════
struct MSRAccess {
    uint32_t msr;
    uint64_t value;
    bool     is_write;
};

// ═══════════════════════════════════════════════════════════════════════════
//  I/O access info — decoded from exit qualification
// ═══════════════════════════════════════════════════════════════════════════
struct IOAccessInfo {
    uint16_t port;
    uint8_t  size;     // 1, 2, or 4 bytes
    bool     is_out;   // true = OUT, false = IN
    bool     is_string;// REP INS/OUTS
    bool     is_rep;
    uint32_t count;    // REP count
};

// ═══════════════════════════════════════════════════════════════════════════
//  VM Exit Handler
// ═══════════════════════════════════════════════════════════════════════════
class VMExitHandler {
public:
    static void Init();

    // ── Main dispatch — called after every VM exit ──
    static VMExitAction HandleExit(vCPU* cpu);
    static VMExitAction HandleSVMExit(vCPU* cpu);  // AMD-specific

    // ── Individual exit handlers ──
    static VMExitAction HandleCPUID(vCPU* cpu);
    static VMExitAction HandleHLT(vCPU* cpu);
    static VMExitAction HandleIO(vCPU* cpu);
    static VMExitAction HandleMSRRead(vCPU* cpu);
    static VMExitAction HandleMSRWrite(vCPU* cpu);
    static VMExitAction HandleCRAccess(vCPU* cpu);
    static VMExitAction HandleExceptionNMI(vCPU* cpu);
    static VMExitAction HandleExternalInterrupt(vCPU* cpu);
    static VMExitAction HandleEPTViolation(vCPU* cpu);
    static VMExitAction HandleEPTMisconfig(vCPU* cpu);
    static VMExitAction HandleTripleFault(vCPU* cpu);
    static VMExitAction HandleVMCall(vCPU* cpu);
    static VMExitAction HandleRDTSC(vCPU* cpu);
    static VMExitAction HandleRDTSCP(vCPU* cpu);
    static VMExitAction HandleINVLPG(vCPU* cpu);
    static VMExitAction HandleTaskSwitch(vCPU* cpu);
    static VMExitAction HandlePreemptTimer(vCPU* cpu);
    static VMExitAction HandleMonitor(vCPU* cpu);
    static VMExitAction HandleMWait(vCPU* cpu);
    static VMExitAction HandlePause(vCPU* cpu);
    static VMExitAction HandleWBINVD(vCPU* cpu);
    static VMExitAction HandleXSETBV(vCPU* cpu);

    // ── CPUID override management ──
    static void AddCPUIDOverride(uint32_t leaf, uint32_t subleaf,
                                  uint32_t eax, uint32_t ebx,
                                  uint32_t ecx, uint32_t edx);

    // ── Guest RIP advancement ──
    static void AdvanceGuestRIP(vCPU* cpu);

    // ── I/O port decode ──
    static IOAccessInfo DecodeIOAccess(uint64_t qualification);

    // ── Statistics ──
    struct ExitStats {
        uint32_t total_exits;
        uint32_t cpuid_exits;
        uint32_t io_exits;
        uint32_t msr_exits;
        uint32_t hlt_exits;
        uint32_t ept_violations;
        uint32_t cr_accesses;
        uint32_t exceptions;
        uint32_t interrupts;
        uint32_t vmcall_exits;
        uint32_t other_exits;
    };
    static const ExitStats& GetStats();
    static void ResetStats();
    static void DumpStats();

private:
    static CPUIDOverride cpuid_overrides[MAX_CPUID_OVERRIDES];
    static int           cpuid_override_count;
    static ExitStats     stats;
    static bool          initialized;
};
