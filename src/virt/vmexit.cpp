// ═══════════════════════════════════════════════════════════════════════════
//  Kurono OS — VM Exit / VM Entry Handler Implementation
//  Comprehensive VM exit dispatch for Intel VT-x and AMD-V
// ═══════════════════════════════════════════════════════════════════════════
#include "vmexit.h"
#include "vmm.h"
#include "ept.h"
#include "hypervisor.h"
#include "../drivers/serial.h"

// ── Static members ──
CPUIDOverride VMExitHandler::cpuid_overrides[MAX_CPUID_OVERRIDES];
int           VMExitHandler::cpuid_override_count = 0;
VMExitHandler::ExitStats VMExitHandler::stats = {};
bool          VMExitHandler::initialized = false;

// ── Inline CPUID ──
static inline void do_cpuid(uint32_t leaf, uint32_t& eax, uint32_t& ebx,
                             uint32_t& ecx, uint32_t& edx) {
    asm volatile("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(leaf), "c"(0));
}

// ═══════════════════════════════════════════════════════════════════════════
//  Init
// ═══════════════════════════════════════════════════════════════════════════

void VMExitHandler::Init() {
    if (initialized) return;
    initialized = true;
    cpuid_override_count = 0;
    ResetStats();

    // Add default CPUID overrides to hide VMX from guest
    // Leaf 1: Clear ECX.VMX (bit 5) and ECX.Hypervisor (bit 31)
    uint32_t eax, ebx, ecx, edx;
    do_cpuid(1, eax, ebx, ecx, edx);
    ecx &= ~CPUID_VMX_BIT;       // Hide VMX from guest
    ecx &= ~CPUID_HYPERVISOR_BIT; // Don't expose hypervisor
    AddCPUIDOverride(1, 0, eax, ebx, ecx, edx);

    SerialLogger::Log("VMExit: Handler initialized with default CPUID overrides\r\n");
}

// ═══════════════════════════════════════════════════════════════════════════
//  Main Dispatch — Intel VT-x
// ═══════════════════════════════════════════════════════════════════════════

VMExitAction VMExitHandler::HandleExit(vCPU* cpu) {
    if (!cpu) return VMEXIT_FATAL;

    stats.total_exits++;
    uint32_t reason = cpu->exit_reason & 0xFFFF; // Lower 16 bits

    switch (reason) {
        case EXIT_REASON_CPUID:
            stats.cpuid_exits++;
            return HandleCPUID(cpu);

        case EXIT_REASON_HLT:
            stats.hlt_exits++;
            return HandleHLT(cpu);

        case EXIT_REASON_IO_INSTR:
            stats.io_exits++;
            return HandleIO(cpu);

        case EXIT_REASON_RDMSR:
            stats.msr_exits++;
            return HandleMSRRead(cpu);

        case EXIT_REASON_WRMSR:
            stats.msr_exits++;
            return HandleMSRWrite(cpu);

        case EXIT_REASON_CR_ACCESS:
            stats.cr_accesses++;
            return HandleCRAccess(cpu);

        case EXIT_REASON_EXCEPTION_NMI:
            stats.exceptions++;
            return HandleExceptionNMI(cpu);

        case EXIT_REASON_EXTERNAL_INT:
            stats.interrupts++;
            return HandleExternalInterrupt(cpu);

        case EXIT_REASON_EPT_VIOLATION:
            stats.ept_violations++;
            return HandleEPTViolation(cpu);

        case EXIT_REASON_EPT_MISCONFIG:
            return HandleEPTMisconfig(cpu);

        case EXIT_REASON_TRIPLE_FAULT:
            return HandleTripleFault(cpu);

        case EXIT_REASON_VMCALL:
            stats.vmcall_exits++;
            return HandleVMCall(cpu);

        case EXIT_REASON_RDTSC:
            return HandleRDTSC(cpu);

        case EXIT_REASON_RDTSCP:
            return HandleRDTSCP(cpu);

        case EXIT_REASON_INVLPG:
            return HandleINVLPG(cpu);

        case EXIT_REASON_TASK_SWITCH:
            return HandleTaskSwitch(cpu);

        case EXIT_REASON_PREEMPT_TIMER:
            return HandlePreemptTimer(cpu);

        case EXIT_REASON_MONITOR:
            return HandleMonitor(cpu);

        case EXIT_REASON_MWAIT:
            return HandleMWait(cpu);

        case EXIT_REASON_PAUSE:
            return HandlePause(cpu);

        case EXIT_REASON_WBINVD:
            return HandleWBINVD(cpu);

        case EXIT_REASON_XSETBV:
            return HandleXSETBV(cpu);

        case EXIT_REASON_INVALID_GUEST:
            SerialLogger::Log("VMExit: INVALID GUEST STATE — fatal\r\n");
            VMM::DumpVCPUState(cpu);
            return VMEXIT_FATAL;

        default:
            stats.other_exits++;
            SerialLogger::Log("VMExit: Unhandled exit reason ");
            SerialLogger::LogDec((int)reason);
            SerialLogger::Log("\r\n");
            return VMEXIT_FATAL;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Main Dispatch — AMD SVM
// ═══════════════════════════════════════════════════════════════════════════

VMExitAction VMExitHandler::HandleSVMExit(vCPU* cpu) {
    if (!cpu || !cpu->vmcb) return VMEXIT_FATAL;

    stats.total_exits++;
    uint64_t exit_code = cpu->vmcb->exit_code;

    switch ((uint32_t)exit_code) {
        case SVM_EXIT_CPUID:
            stats.cpuid_exits++;
            return HandleCPUID(cpu);

        case SVM_EXIT_HLT:
            stats.hlt_exits++;
            return HandleHLT(cpu);

        case SVM_EXIT_IOIO:
            stats.io_exits++;
            return HandleIO(cpu);

        case SVM_EXIT_MSR:
            stats.msr_exits++;
            // exit_info1 bit 0: 0 = RDMSR, 1 = WRMSR
            if (cpu->vmcb->exit_info1 & 1)
                return HandleMSRWrite(cpu);
            else
                return HandleMSRRead(cpu);

        case SVM_EXIT_VMMCALL:
            stats.vmcall_exits++;
            return HandleVMCall(cpu);

        case SVM_EXIT_NPF:
            stats.ept_violations++;
            return HandleEPTViolation(cpu); // NPF uses same handler

        default:
            stats.other_exits++;
            SerialLogger::Log("SVM Exit: Unhandled code ");
            SerialLogger::LogHex((uint32_t)exit_code);
            SerialLogger::Log("\r\n");
            return VMEXIT_FATAL;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  CPUID Exit — intercept, optionally override, advance RIP
// ═══════════════════════════════════════════════════════════════════════════

VMExitAction VMExitHandler::HandleCPUID(vCPU* cpu) {
    uint32_t leaf = cpu->regs[0]; // EAX = leaf
    uint32_t subleaf = cpu->regs[1]; // ECX = subleaf

    // Check for override
    for (int i = 0; i < cpuid_override_count; i++) {
        if (cpuid_overrides[i].active &&
            cpuid_overrides[i].leaf == leaf &&
            cpuid_overrides[i].subleaf == subleaf) {
            cpu->regs[0] = cpuid_overrides[i].eax;
            cpu->regs[1] = cpuid_overrides[i].ecx;
            cpu->regs[2] = cpuid_overrides[i].edx;
            cpu->regs[3] = cpuid_overrides[i].ebx;
            AdvanceGuestRIP(cpu);
            return VMEXIT_HANDLED;
        }
    }

    // No override — execute real CPUID and pass through
    uint32_t eax, ebx, ecx, edx;
    asm volatile("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(leaf), "c"(subleaf));

    // Always hide VMX/SVM capability from guest
    if (leaf == 1) {
        ecx &= ~CPUID_VMX_BIT;
        ecx &= ~CPUID_HYPERVISOR_BIT;
    }

    cpu->regs[0] = eax; // EAX
    cpu->regs[1] = ecx; // ECX
    cpu->regs[2] = edx; // EDX
    cpu->regs[3] = ebx; // EBX

    AdvanceGuestRIP(cpu);
    return VMEXIT_HANDLED;
}

// ═══════════════════════════════════════════════════════════════════════════
//  HLT Exit — guest executed HLT
// ═══════════════════════════════════════════════════════════════════════════

VMExitAction VMExitHandler::HandleHLT(vCPU* cpu) {
    SerialLogger::Log("VMExit: Guest HLT\r\n");
    // In a real hypervisor, we'd yield this vCPU until next interrupt
    // For now, advance past HLT and re-enter
    AdvanceGuestRIP(cpu);
    return VMEXIT_HANDLED;
}

// ═══════════════════════════════════════════════════════════════════════════
//  I/O Instruction Exit — port I/O interception
// ═══════════════════════════════════════════════════════════════════════════

VMExitAction VMExitHandler::HandleIO(vCPU* cpu) {
    IOAccessInfo io = DecodeIOAccess(cpu->exit_qualification);

    // Route all I/O through the Hypervisor device layer
    uint32_t value = 0;
    if (!io.is_out) {
        // IN instruction: guest is reading a port
        value = 0; // Default value
    } else {
        // OUT instruction: value is in AL/AX/EAX
        value = cpu->regs[0]; // EAX
        if (io.size == 1) value &= 0xFF;
        else if (io.size == 2) value &= 0xFFFF;
    }

    bool handled = Hypervisor::HandleGuestIO(io.port, io.is_out, io.size, value);

    if (!io.is_out && handled) {
        // Store read result back to guest EAX
        if (io.size == 1) {
            cpu->regs[0] = (cpu->regs[0] & 0xFFFFFF00) | (value & 0xFF);
        } else if (io.size == 2) {
            cpu->regs[0] = (cpu->regs[0] & 0xFFFF0000) | (value & 0xFFFF);
        } else {
            cpu->regs[0] = value;
        }
    }

    if (!handled) {
        // Unhandled port — log it for debugging
        SerialLogger::Log("VMExit: Unhandled I/O port=0x");
        SerialLogger::LogHex(io.port);
        SerialLogger::Log(io.is_out ? " OUT" : " IN");
        SerialLogger::Log(" size=");
        SerialLogger::LogDec(io.size);
        SerialLogger::Log("\r\n");
        // Return 0xFF for IN from unhandled port
        if (!io.is_out) {
            if (io.size == 1) cpu->regs[0] = (cpu->regs[0] & 0xFFFFFF00) | 0xFF;
            else if (io.size == 2) cpu->regs[0] = (cpu->regs[0] & 0xFFFF0000) | 0xFFFF;
            else cpu->regs[0] = 0xFFFFFFFF;
        }
    }

    AdvanceGuestRIP(cpu);
    return VMEXIT_HANDLED;
}

IOAccessInfo VMExitHandler::DecodeIOAccess(uint64_t qualification) {
    IOAccessInfo io;
    io.size = (uint8_t)((qualification & 0x07) + 1); // bits [2:0] = size - 1
    io.is_out = (qualification & (1 << 3)) == 0;     // bit 3: 0 = OUT, 1 = IN
    io.is_string = (qualification & (1 << 4)) != 0;  // bit 4: string (INS/OUTS)
    io.is_rep = (qualification & (1 << 5)) != 0;     // bit 5: REP prefix
    io.port = (uint16_t)((qualification >> 16) & 0xFFFF); // bits [31:16]
    io.count = io.is_rep ? 1 : 1; // ECX for REP
    return io;
}

// ═══════════════════════════════════════════════════════════════════════════
//  MSR Read/Write Exits
// ═══════════════════════════════════════════════════════════════════════════

VMExitAction VMExitHandler::HandleMSRRead(vCPU* cpu) {
    uint32_t msr = cpu->regs[1]; // ECX = MSR address

    SerialLogger::Log("VMExit: RDMSR ");
    SerialLogger::LogHex(msr);
    SerialLogger::Log("\r\n");

    // Emulate common MSRs
    uint64_t value = 0;
    switch (msr) {
        case 0x10:  // IA32_TIME_STAMP_COUNTER
        {
            uint32_t lo, hi;
            asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
            value = ((uint64_t)hi << 32) | lo;
            break;
        }
        case 0x1B:  // IA32_APIC_BASE
            value = 0xFEE00900; // Default APIC base, BSP, enabled
            break;
        case 0xC0000080: // IA32_EFER
            value = 0; // Guest in 32-bit mode, no long mode
            break;
        case 0x174: // IA32_SYSENTER_CS
        case 0x175: // IA32_SYSENTER_ESP
        case 0x176: // IA32_SYSENTER_EIP
            value = 0; // Not configured
            break;
        default:
            // Return 0 for unknown MSRs rather than #GP
            value = 0;
            break;
    }

    cpu->regs[0] = (uint32_t)(value & 0xFFFFFFFF);  // EAX = low 32
    cpu->regs[2] = (uint32_t)(value >> 32);           // EDX = high 32

    AdvanceGuestRIP(cpu);
    return VMEXIT_HANDLED;
}

VMExitAction VMExitHandler::HandleMSRWrite(vCPU* cpu) {
    uint32_t msr = cpu->regs[1]; // ECX
    uint64_t value = ((uint64_t)cpu->regs[2] << 32) | cpu->regs[0]; // EDX:EAX

    SerialLogger::Log("VMExit: WRMSR ");
    SerialLogger::LogHex(msr);
    SerialLogger::Log(" = ");
    SerialLogger::LogHex((uint32_t)(value >> 32));
    SerialLogger::LogHex((uint32_t)value);
    SerialLogger::Log("\r\n");

    // Silently absorb most MSR writes (they affect virtual state only)
    switch (msr) {
        case 0x1B:  // IA32_APIC_BASE — store for virtual APIC
            break;
        case 0xC0000080: // IA32_EFER
            break;
        default:
            break;
    }

    AdvanceGuestRIP(cpu);
    return VMEXIT_HANDLED;
}

// ═══════════════════════════════════════════════════════════════════════════
//  CR Access
// ═══════════════════════════════════════════════════════════════════════════

VMExitAction VMExitHandler::HandleCRAccess(vCPU* cpu) {
    uint64_t qual = cpu->exit_qualification;
    uint32_t cr_num = (uint32_t)(qual & 0x0F);
    uint32_t access_type = (uint32_t)((qual >> 4) & 0x03); // 0=MOV to, 1=MOV from, 2=CLTS, 3=LMSW
    uint32_t reg = (uint32_t)((qual >> 8) & 0x0F);

    SerialLogger::Log("VMExit: CR");
    SerialLogger::LogDec(cr_num);
    SerialLogger::Log(access_type == 0 ? " write" : " read");
    SerialLogger::Log(" reg=");
    SerialLogger::LogDec(reg);
    SerialLogger::Log("\r\n");

    // For CR0 writes, we might need to update shadow CR0
    // For CR3 writes, we might need to flush TLB
    // For CR4 writes, check for mode-changing bits

    AdvanceGuestRIP(cpu);
    return VMEXIT_HANDLED;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Exceptions & Interrupts
// ═══════════════════════════════════════════════════════════════════════════

VMExitAction VMExitHandler::HandleExceptionNMI(vCPU* cpu) {
    // Read interrupt information from VMCS
    uint32_t int_info = 0;
    if (cpu->type == VIRT_INTEL_VTX) {
        int_info = VMM::VMRead(VMCS_EXIT_INT_INFO);
    }

    uint8_t vector = int_info & 0xFF;
    uint8_t type = (int_info >> 8) & 0x07;
    bool has_error = (int_info >> 11) & 0x01;

    SerialLogger::Log("VMExit: Exception vector=");
    SerialLogger::LogDec(vector);
    SerialLogger::Log(" type=");
    SerialLogger::LogDec(type);
    if (has_error) {
        SerialLogger::Log(" error=");
        SerialLogger::LogHex(VMM::VMRead(VMCS_EXIT_INT_ERROR));
    }
    SerialLogger::Log("\r\n");

    // Reflect exception back into guest via event injection
    // This is simplified — real implementation would check if guest can handle it
    return VMEXIT_HANDLED;
}

VMExitAction VMExitHandler::HandleExternalInterrupt(vCPU* cpu) {
    (void)cpu;
    // External interrupt (from host) — handle in host, then re-enter guest
    return VMEXIT_CONTINUE;
}

// ═══════════════════════════════════════════════════════════════════════════
//  EPT Violation
// ═══════════════════════════════════════════════════════════════════════════

VMExitAction VMExitHandler::HandleEPTViolation(vCPU* cpu) {
    uint64_t guest_phys = 0;
    uint64_t qualification = cpu->exit_qualification;

    if (cpu->type == VIRT_INTEL_VTX) {
        guest_phys = (uint64_t)VMM::VMRead(VMCS_GUEST_PHYS_ADDR);
    } else if (cpu->type == VIRT_AMD_SVM && cpu->vmcb) {
        guest_phys = cpu->vmcb->exit_info2; // NPF stores faulting GPA in exit_info2
        qualification = cpu->vmcb->exit_info1;
    }

    // Check if this is an MMIO access — route to Hypervisor device layer
    bool is_write = (qualification & 0x02) != 0; // Bit 1 = write access
    uint32_t mmio_value = 0;
    if (is_write) {
        // For MMIO writes, the value is often in EAX
        mmio_value = cpu->regs[0];
    }
    if (Hypervisor::HandleGuestMMIO(guest_phys, is_write, 4, mmio_value)) {
        if (!is_write) {
            cpu->regs[0] = mmio_value; // Return read value
        }
        AdvanceGuestRIP(cpu);
        return VMEXIT_HANDLED;
    }

    // Try EPT/NPT resolution
    bool handled;
    if (cpu->type == VIRT_AMD_SVM)
        handled = EPTManager::HandleNPF(guest_phys, qualification);
    else
        handled = EPTManager::HandleEPTViolation(guest_phys, qualification);

    if (handled) return VMEXIT_HANDLED;

    SerialLogger::Log("VMExit: Unresolvable EPT violation at GPA=0x");
    SerialLogger::LogHex((uint32_t)guest_phys);
    SerialLogger::Log(" qual=0x");
    SerialLogger::LogHex((uint32_t)qualification);
    SerialLogger::Log(" — shutting down guest\r\n");
    VMM::DumpVCPUState(cpu);
    return VMEXIT_SHUTDOWN;
}

VMExitAction VMExitHandler::HandleEPTMisconfig(vCPU* cpu) {
    uint64_t guest_phys = 0;
    if (cpu->type == VIRT_INTEL_VTX) {
        guest_phys = (uint64_t)VMM::VMRead(VMCS_GUEST_PHYS_ADDR);
    }
    SerialLogger::Log("VMExit: EPT MISCONFIGURATION at GPA ");
    SerialLogger::LogHex((uint32_t)(guest_phys >> 32));
    SerialLogger::LogHex((uint32_t)guest_phys);
    SerialLogger::Log(" — FATAL\r\n");
    return VMEXIT_FATAL;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Triple Fault — catastrophic guest failure
// ═══════════════════════════════════════════════════════════════════════════

VMExitAction VMExitHandler::HandleTripleFault(vCPU* cpu) {
    SerialLogger::Log("VMExit: TRIPLE FAULT — guest crashed\r\n");
    VMM::DumpVCPUState(cpu);
    return VMEXIT_REBOOT;
}

// ═══════════════════════════════════════════════════════════════════════════
//  VMCALL — hypercall from guest
// ═══════════════════════════════════════════════════════════════════════════

VMExitAction VMExitHandler::HandleVMCall(vCPU* cpu) {
    uint32_t call_num = cpu->regs[0]; // EAX = hypercall number
    uint32_t arg1 = cpu->regs[3];     // EBX = arg1
    uint32_t arg2 = cpu->regs[1];     // ECX = arg2

    SerialLogger::Log("VMExit: VMCALL #");
    SerialLogger::LogDec(call_num);
    SerialLogger::Log(" arg1=");
    SerialLogger::LogHex(arg1);
    SerialLogger::Log(" arg2=");
    SerialLogger::LogHex(arg2);
    SerialLogger::Log("\r\n");

    // Hypercall dispatch
    switch (call_num) {
        case 0: // NOP / heartbeat
            cpu->regs[0] = 0; // Return success
            break;
        case 1: // Get hypervisor info
            cpu->regs[0] = 0x4B55524F; // "KURO"
            cpu->regs[3] = 0x4E4F5320; // "NO S"
            break;
        case 2: // Request shutdown
            return VMEXIT_SHUTDOWN;
        case 3: // Request reboot
            return VMEXIT_REBOOT;
        default:
            cpu->regs[0] = 0xFFFFFFFF; // Unknown
            break;
    }

    AdvanceGuestRIP(cpu);
    return VMEXIT_HANDLED;
}

// ═══════════════════════════════════════════════════════════════════════════
//  RDTSC / RDTSCP — timestamp counter access
// ═══════════════════════════════════════════════════════════════════════════

VMExitAction VMExitHandler::HandleRDTSC(vCPU* cpu) {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    cpu->regs[0] = lo; // EAX
    cpu->regs[2] = hi; // EDX
    AdvanceGuestRIP(cpu);
    return VMEXIT_HANDLED;
}

VMExitAction VMExitHandler::HandleRDTSCP(vCPU* cpu) {
    // Same as RDTSC but also set ECX to IA32_TSC_AUX
    HandleRDTSC(cpu);
    cpu->regs[1] = 0; // ECX = TSC_AUX (processor ID typically)
    return VMEXIT_HANDLED;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Miscellaneous exits
// ═══════════════════════════════════════════════════════════════════════════

VMExitAction VMExitHandler::HandleINVLPG(vCPU* cpu) {
    // Guest invalidated a TLB page — we could invalidate our EPT mapping
    AdvanceGuestRIP(cpu);
    return VMEXIT_HANDLED;
}

VMExitAction VMExitHandler::HandleTaskSwitch(vCPU* cpu) {
    SerialLogger::Log("VMExit: Task switch\r\n");
    (void)cpu;
    return VMEXIT_HANDLED;
}

VMExitAction VMExitHandler::HandlePreemptTimer(vCPU* cpu) {
    (void)cpu;
    // VMX preemption timer fired — time slice expired
    return VMEXIT_CONTINUE;
}

VMExitAction VMExitHandler::HandleMonitor(vCPU* cpu) {
    AdvanceGuestRIP(cpu);
    return VMEXIT_HANDLED;
}

VMExitAction VMExitHandler::HandleMWait(vCPU* cpu) {
    AdvanceGuestRIP(cpu);
    return VMEXIT_HANDLED;
}

VMExitAction VMExitHandler::HandlePause(vCPU* cpu) {
    AdvanceGuestRIP(cpu);
    return VMEXIT_CONTINUE;
}

VMExitAction VMExitHandler::HandleWBINVD(vCPU* cpu) {
    // Write-back and invalidate cache — expensive, just skip
    AdvanceGuestRIP(cpu);
    return VMEXIT_HANDLED;
}

VMExitAction VMExitHandler::HandleXSETBV(vCPU* cpu) {
    // Extended control register write — used for AVX state
    AdvanceGuestRIP(cpu);
    return VMEXIT_HANDLED;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Guest RIP Advancement
// ═══════════════════════════════════════════════════════════════════════════

void VMExitHandler::AdvanceGuestRIP(vCPU* cpu) {
    if (!cpu) return;

    if (cpu->type == VIRT_INTEL_VTX) {
        uint32_t instr_len = VMM::VMRead(VMCS_EXIT_INSTR_LENGTH);
        uint32_t rip = VMM::VMRead(VMCS_GUEST_RIP);
        VMM::VMWrite(VMCS_GUEST_RIP, rip + instr_len);
    } else if (cpu->type == VIRT_AMD_SVM && cpu->vmcb) {
        // SVM gives us next_rip directly
        if (cpu->vmcb->next_rip != 0) {
            cpu->vmcb->rip = cpu->vmcb->next_rip;
        } else {
            // Fallback: some exits don't set next_rip, advance by 2 (typical for I/O)
            cpu->vmcb->rip += 2;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  CPUID Override Management
// ═══════════════════════════════════════════════════════════════════════════

void VMExitHandler::AddCPUIDOverride(uint32_t leaf, uint32_t subleaf,
                                      uint32_t eax, uint32_t ebx,
                                      uint32_t ecx, uint32_t edx) {
    if (cpuid_override_count >= MAX_CPUID_OVERRIDES) return;
    CPUIDOverride& ov = cpuid_overrides[cpuid_override_count++];
    ov.leaf = leaf;
    ov.subleaf = subleaf;
    ov.eax = eax;
    ov.ebx = ebx;
    ov.ecx = ecx;
    ov.edx = edx;
    ov.active = true;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Statistics
// ═══════════════════════════════════════════════════════════════════════════

const VMExitHandler::ExitStats& VMExitHandler::GetStats() { return stats; }

void VMExitHandler::ResetStats() {
    stats.total_exits = 0;
    stats.cpuid_exits = 0;
    stats.io_exits = 0;
    stats.msr_exits = 0;
    stats.hlt_exits = 0;
    stats.ept_violations = 0;
    stats.cr_accesses = 0;
    stats.exceptions = 0;
    stats.interrupts = 0;
    stats.vmcall_exits = 0;
    stats.other_exits = 0;
}

void VMExitHandler::DumpStats() {
    SerialLogger::Log("=== VM Exit Statistics ===\r\n");
    SerialLogger::Log("  Total exits:    "); SerialLogger::LogDec(stats.total_exits); SerialLogger::Log("\r\n");
    SerialLogger::Log("  CPUID:          "); SerialLogger::LogDec(stats.cpuid_exits); SerialLogger::Log("\r\n");
    SerialLogger::Log("  I/O:            "); SerialLogger::LogDec(stats.io_exits); SerialLogger::Log("\r\n");
    SerialLogger::Log("  MSR:            "); SerialLogger::LogDec(stats.msr_exits); SerialLogger::Log("\r\n");
    SerialLogger::Log("  HLT:            "); SerialLogger::LogDec(stats.hlt_exits); SerialLogger::Log("\r\n");
    SerialLogger::Log("  EPT violations: "); SerialLogger::LogDec(stats.ept_violations); SerialLogger::Log("\r\n");
    SerialLogger::Log("  CR accesses:    "); SerialLogger::LogDec(stats.cr_accesses); SerialLogger::Log("\r\n");
    SerialLogger::Log("  Exceptions:     "); SerialLogger::LogDec(stats.exceptions); SerialLogger::Log("\r\n");
    SerialLogger::Log("  Interrupts:     "); SerialLogger::LogDec(stats.interrupts); SerialLogger::Log("\r\n");
    SerialLogger::Log("  VMCALLs:        "); SerialLogger::LogDec(stats.vmcall_exits); SerialLogger::Log("\r\n");
    SerialLogger::Log("  Other:          "); SerialLogger::LogDec(stats.other_exits); SerialLogger::Log("\r\n");
}
