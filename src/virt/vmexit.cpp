//  kurono os - vm exit / vm entry handler implementation
//  comprehensive vm exit dispatch for intel vt-x and amd-v
#include "vmexit.h"
#include "vmm.h"
#include "ept.h"
#include "hypervisor.h"
#include "guest_mem.h"
#include "../drivers/serial.h"
#include "../drivers/graphics.h"
#include "../drivers/audio.h"
#include "../drivers/audio_server.h"
#include "../drivers/keyboard.h"
#include "../drivers/mouse.h"
#include "../linux/linux_devices.h"
#include "v9fs.h"
#include "../security/ksa.h"   // ksa restricted result channel (vmcall 0x4b) (satoru)

CPUIDOverride VMExitHandler::cpuid_overrides[MAX_CPUID_OVERRIDES];
int           VMExitHandler::cpuid_override_count = 0;
VMExitHandler::ExitStats VMExitHandler::stats = {};
bool          VMExitHandler::initialized = false;

static inline void do_cpuid(uint32_t leaf, uint32_t& eax, uint32_t& ebx,
                             uint32_t& ecx, uint32_t& edx) {
    asm volatile("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(leaf), "c"(0));
}

//  init

void VMExitHandler::Init() {
    if (initialized) return;
    initialized = true;
    cpuid_override_count = 0;
    ResetStats();

    // add default cpuid overrides to hide vmx from guest
    // leaf 1: clear ecx.vmx (bit 5) and ecx.hypervisor (bit 31)
    uint32_t eax, ebx, ecx, edx;
    do_cpuid(1, eax, ebx, ecx, edx);
    ecx &= ~CPUID_VMX_BIT;       // hide vmx from guest
    ecx &= ~CPUID_HYPERVISOR_BIT; // don't expose hypervisor
    AddCPUIDOverride(1, 0, eax, ebx, ecx, edx);

    SerialLogger::Log("VMExit: Handler initialized with default CPUID overrides\r\n");
}

//  main dispatch - intel vt-x

VMExitAction VMExitHandler::HandleExit(vCPU* cpu) {
    if (!cpu) return VMEXIT_FATAL;

    stats.total_exits++;
    uint32_t reason = cpu->exit_reason & 0xFFFF; // lower 16 bits

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
            SerialLogger::Log("VMExit: INVALID GUEST STATE - fatal\r\n");
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

//  main dispatch - amd svm

VMExitAction VMExitHandler::HandleSVMExit(vCPU* cpu) {
    if (!cpu || !cpu->vmcb) return VMEXIT_FATAL;

    stats.total_exits++;
    uint64_t exit_code = cpu->vmcb->exit_code;

    if (exit_code <= 0x000F) {
        stats.cr_exits++;
        return HandleSVMCRAccess(cpu, (uint32_t)exit_code, false);
    }
    if (exit_code >= 0x0010 && exit_code <= 0x001F) {
        stats.cr_exits++;
        return HandleSVMCRAccess(cpu, (uint32_t)(exit_code - 0x10), true);
    }
    if (exit_code >= 0x0040 && exit_code <= 0x005F) {
        stats.exception_exits++;
        uint32_t vector = (uint32_t)(exit_code - 0x40);
        // re-inject the exception into the guest via vmcb event_inject
        uint64_t inject = (uint64_t)vector | (3ULL << 8) | (1ULL << 31);
        // #pf (14), #df (8), #ts (10), #np (11), #ss (12), #gp (13), #ac (17) have error codes
        if (vector == 8 || (vector >= 10 && vector <= 14) || vector == 17) {
            inject |= (1ULL << 11); // has error code
            cpu->vmcb->event_inject = inject;
            // error code is in exit_info1
        } else {
            cpu->vmcb->event_inject = inject;
        }
        return VMEXIT_HANDLED;
    }

    switch ((uint32_t)exit_code) {
        case SVM_EXIT_INTR:
            stats.interrupt_exits++;
            return VMEXIT_HANDLED; // external interrupt, just continue

        case SVM_EXIT_NMI:
            return VMEXIT_HANDLED; // nmi - continue

        case SVM_EXIT_VINTR:
            return VMEXIT_HANDLED; // virtual interrupt - continue

        case SVM_EXIT_RDTSC:
        case 0x0087: // rdtscp
        {
            // pass through host tsc to guest
            uint32_t lo, hi;
            asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
            cpu->regs[0] = lo;  // eax
            cpu->regs[2] = hi;  // edx
            if (exit_code == 0x0087) cpu->regs[1] = 0; // ecx for rdtscp
            AdvanceGuestRIP(cpu);
            return VMEXIT_HANDLED;
        }

        case SVM_EXIT_PUSHF:
        case SVM_EXIT_POPF:
            // pushf/popf - shouldn't normally be intercepted, but if so, skip
            AdvanceGuestRIP(cpu);
            return VMEXIT_HANDLED;

        case SVM_EXIT_CPUID:
            stats.cpuid_exits++;
            return HandleCPUID(cpu);

        case SVM_EXIT_INVLPG:
            // invalidate page - just continue
            AdvanceGuestRIP(cpu);
            return VMEXIT_HANDLED;

        case SVM_EXIT_INVD:
            // invd/wbinvd - skip
            AdvanceGuestRIP(cpu);
            return VMEXIT_HANDLED;

        case SVM_EXIT_HLT:
            stats.hlt_exits++;
            return HandleHLT(cpu);

        case SVM_EXIT_IOIO:
            stats.io_exits++;
            return HandleIO(cpu);

        case SVM_EXIT_MSR:
            stats.msr_exits++;
            if (cpu->vmcb->exit_info1 & 1)
                return HandleMSRWrite(cpu);
            else
                return HandleMSRRead(cpu);

        case SVM_EXIT_SHUTDOWN:
            SerialLogger::Log("SVM Exit: SHUTDOWN (triple fault)\r\n");
            return VMEXIT_SHUTDOWN;

        case SVM_EXIT_VMMCALL:
            stats.vmcall_exits++;
            return HandleVMCall(cpu);

        case SVM_EXIT_NPF:
            stats.ept_violations++;
            return HandleEPTViolation(cpu);

        default:
            stats.other_exits++;
            // non-critical unknown exits: log but continue
            if (stats.other_exits <= 5) {
                SerialLogger::Log("SVM Exit: Unknown code ");
                SerialLogger::LogHex((uint32_t)exit_code);
                SerialLogger::Log(" at RIP=");
                SerialLogger::LogHex((uint32_t)cpu->vmcb->rip);
                SerialLogger::Log(" - skipping\r\n");
            }
            AdvanceGuestRIP(cpu);
            return VMEXIT_HANDLED;
    }
}

//  svm cr access handler

VMExitAction VMExitHandler::HandleSVMCRAccess(vCPU* cpu, uint32_t cr_num, bool is_write) {
    VMCB* vmcb = cpu->vmcb;

    if (is_write) {
        // guest writing to cr - exit_info1 has the new value on some cpus,
        // but standard svm doesn't provide it directly. we decode from next_rip.
        // for cr0 writes during linux boot: read the value from the gpr that
        // the guest used. since we can't decode the instruction portably,
        // use exit_info1 which holds the gp register value for mov-to-cr.
        uint64_t val = cpu->vmcb->exit_info1;

        switch (cr_num) {
            case 0:
                vmcb->cr0 = val | 0x20; // keep ne set
                break;
            case 3:
                vmcb->cr3 = val;
                vmcb->tlb_control = 1; // flush tlb on cr3 change
                break;
            case 4:
                vmcb->cr4 = val;
                break;
            default:
                break;
        }
    } else {
        // guest reading cr - put the value in a gpr
        // on svm, exit_info1 for cr reads gives the gpr number (0=rax etc.)
        uint64_t val = 0;
        switch (cr_num) {
            case 0: val = vmcb->cr0; break;
            case 2: val = vmcb->cr2; break;
            case 3: val = vmcb->cr3; break;
            case 4: val = vmcb->cr4; break;
            default: break;
        }
        // for cr0 reads, the guest typically uses mov rx, cr0
        // svm doesn't encode the destination register in exit_info for cr reads.
        // the vmcb auto-handles cr0/cr3/cr4 reads with nested paging
        // if the intercept is not set. since we intercepted, put value in rax.
        vmcb->rax = val;
        cpu->regs[0] = val;
    }

    AdvanceGuestRIP(cpu);
    return VMEXIT_HANDLED;
}

//  cpuid exit - intercept, optionally override, advance rip

VMExitAction VMExitHandler::HandleCPUID(vCPU* cpu) {
    uint32_t leaf = cpu->regs[0]; // eax = leaf
    uint32_t subleaf = cpu->regs[1]; // ecx = subleaf

    // check for override
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

    // no override - execute real cpuid and pass through
    uint32_t eax, ebx, ecx, edx;
    asm volatile("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(leaf), "c"(subleaf));

    // always hide vmx/svm capability from guest
    if (leaf == 1) {
        ecx &= ~CPUID_VMX_BIT;
        ecx &= ~CPUID_HYPERVISOR_BIT;
    }

    cpu->regs[0] = eax; // eax
    cpu->regs[1] = ecx; // ecx
    cpu->regs[2] = edx; // edx
    cpu->regs[3] = ebx; // ebx

    AdvanceGuestRIP(cpu);
    return VMEXIT_HANDLED;
}

//  hlt exit - guest executed hlt

VMExitAction VMExitHandler::HandleHLT(vCPU* cpu) {
    SerialLogger::Log("VMExit: Guest HLT\r\n");
    // in a real hypervisor, we'd yield this vcpu until next interrupt
    // for now, advance past hlt and re-enter
    AdvanceGuestRIP(cpu);
    return VMEXIT_HANDLED;
}

//  i/o instruction exit - port i/o interception

VMExitAction VMExitHandler::HandleIO(vCPU* cpu) {
    IOAccessInfo io;

    if (cpu->type == VIRT_AMD_SVM) {
        // svm ioio exit_info1 format:
        //   bit 0: type (1=in, 0=out)
        //   bit 4: str, bit 5: rep
        //   bit 6: sz8, bit 7: sz16, bit 8: sz32
        //   bits 16-31: port
        uint64_t info = cpu->vmcb->exit_info1;
        io.is_out = !(info & 1);
        io.is_string = (info & (1 << 4)) != 0;
        io.is_rep = (info & (1 << 5)) != 0;
        if (info & (1 << 6)) io.size = 1;
        else if (info & (1 << 7)) io.size = 2;
        else if (info & (1 << 8)) io.size = 4;
        else io.size = 1;
        io.port = (uint16_t)((info >> 16) & 0xFFFF);
        io.count = 1;
    } else {
        // intel vt-x format
        io = DecodeIOAccess(cpu->exit_qualification);
    }

    // route all i/o through the hypervisor device layer
    uint32_t value = 0;
    if (!io.is_out) {
        // in instruction: guest is reading a port
        value = 0; // default value
    } else {
        // out instruction: value is in al/ax/eax
        value = cpu->regs[0]; // eax
        if (io.size == 1) value &= 0xFF;
        else if (io.size == 2) value &= 0xFFFF;
    }

    bool handled = Hypervisor::HandleGuestIO(io.port, io.is_out, io.size, value);

    if (!io.is_out && handled) {
        // store read result back to guest eax
        if (io.size == 1) {
            cpu->regs[0] = (cpu->regs[0] & 0xFFFFFF00) | (value & 0xFF);
        } else if (io.size == 2) {
            cpu->regs[0] = (cpu->regs[0] & 0xFFFF0000) | (value & 0xFFFF);
        } else {
            cpu->regs[0] = value;
        }
    }

    if (!handled) {
        // unhandled port - log it for debugging
        SerialLogger::Log("VMExit: Unhandled I/O port=0x");
        SerialLogger::LogHex(io.port);
        SerialLogger::Log(io.is_out ? " OUT" : " IN");
        SerialLogger::Log(" size=");
        SerialLogger::LogDec(io.size);
        SerialLogger::Log("\r\n");
        // return 0xff for in from unhandled port
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
    io.is_out = (qualification & (1 << 3)) == 0;     // bit 3: 0 = out, 1 = in
    io.is_string = (qualification & (1 << 4)) != 0;  // bit 4: string (ins/outs)
    io.is_rep = (qualification & (1 << 5)) != 0;     // bit 5: rep prefix
    io.port = (uint16_t)((qualification >> 16) & 0xFFFF); // bits [31:16]
    io.count = io.is_rep ? 1 : 1; // ecx for rep
    return io;
}

//  msr read/write exits

VMExitAction VMExitHandler::HandleMSRRead(vCPU* cpu) {
    uint32_t msr = cpu->regs[1]; // ecx = msr address

    SerialLogger::Log("VMExit: RDMSR ");
    SerialLogger::LogHex(msr);
    SerialLogger::Log("\r\n");

    // emulate common msrs
    uint64_t value = 0;
    switch (msr) {
        case 0x10:  // ia32_time_stamp_counter
        {
            uint32_t lo, hi;
            asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
            value = ((uint64_t)hi << 32) | lo;
            break;
        }
        case 0x1B:  // ia32_apic_base
            value = 0xFEE00900; // default apic base, bsp, enabled
            break;
        case 0xC0000080: // ia32_efer
            value = 0; // guest in 32-bit mode, no long mode
            break;
        case 0x174: // ia32_sysenter_cs
        case 0x175: // ia32_sysenter_esp
        case 0x176: // ia32_sysenter_eip
            value = 0; // not configured
            break;
        default:
            // return 0 for unknown msrs rather than #gp
            value = 0;
            break;
    }

    cpu->regs[0] = (uint32_t)(value & 0xFFFFFFFF);  // eax = low 32
    cpu->regs[2] = (uint32_t)(value >> 32);           // edx = high 32

    AdvanceGuestRIP(cpu);
    return VMEXIT_HANDLED;
}

VMExitAction VMExitHandler::HandleMSRWrite(vCPU* cpu) {
    uint32_t msr = cpu->regs[1]; // ecx
    uint64_t value = ((uint64_t)cpu->regs[2] << 32) | cpu->regs[0]; // edx:eax

    SerialLogger::Log("VMExit: WRMSR ");
    SerialLogger::LogHex(msr);
    SerialLogger::Log(" = ");
    SerialLogger::LogHex((uint32_t)(value >> 32));
    SerialLogger::LogHex((uint32_t)value);
    SerialLogger::Log("\r\n");

    // silently absorb most msr writes (they affect virtual state only)
    switch (msr) {
        case 0x1B:  // ia32_apic_base - store for virtual apic
            break;
        case 0xC0000080: // ia32_efer
            break;
        default:
            break;
    }

    AdvanceGuestRIP(cpu);
    return VMEXIT_HANDLED;
}

//  cr access

VMExitAction VMExitHandler::HandleCRAccess(vCPU* cpu) {
    uint64_t qual = cpu->exit_qualification;
    uint32_t cr_num = (uint32_t)(qual & 0x0F);
    uint32_t access_type = (uint32_t)((qual >> 4) & 0x03); // 0=mov to, 1=mov from, 2=clts, 3=lmsw
    uint32_t reg = (uint32_t)((qual >> 8) & 0x0F);

    SerialLogger::Log("VMExit: CR");
    SerialLogger::LogDec(cr_num);
    SerialLogger::Log(access_type == 0 ? " write" : " read");
    SerialLogger::Log(" reg=");
    SerialLogger::LogDec(reg);
    SerialLogger::Log("\r\n");

    // map register index to vcpu regs array
    // intel sdm: 0=rax,1=rcx,2=rdx,3=rbx,4=rsp,5=rbp,6=rsi,7=rdi,8-15=r8-r15
    // regs[] uses the same intel standard (modrm) ordering.
    auto GetGPR = [&](uint32_t r) -> uint64_t {
        if (r == 4) return VMM::VMRead(VMCS_GUEST_RSP);
        if (r < 16) return cpu->regs[r];
        return 0;
    };
    auto SetGPR = [&](uint32_t r, uint64_t val) {
        if (r == 4) VMM::VMWrite(VMCS_GUEST_RSP, val);
        else if (r < 16) cpu->regs[r] = val;
    };

    if (access_type == 0) {
        // mov to crn - guest is writing
        uint64_t val = GetGPR(reg);
        switch (cr_num) {
            case 0: {
                // cr0 write: update guest cr0 and its shadow
                // preserve host-required bits (ne must stay set for vmcs validity)
                val |= 0x20;  // ne must be set
                VMM::VMWrite(VMCS_GUEST_CR0, val);
                VMM::VMWrite(VMCS_CR0_READ_SHADOW, val);
                break;
            }
            case 3:
                // cr3 write: page table base change - update and flush guest tlb
                VMM::VMWrite(VMCS_GUEST_CR3, val);
                break;
            case 4:
                // cr4 write: update and shadow
                VMM::VMWrite(VMCS_GUEST_CR4, val);
                VMM::VMWrite(VMCS_CR4_READ_SHADOW, val);
                break;
            default:
                break;
        }
    } else if (access_type == 1) {
        // mov from crn - guest is reading
        uint64_t val = 0;
        switch (cr_num) {
            case 0: val = VMM::VMRead(VMCS_GUEST_CR0); break;
            case 3: val = VMM::VMRead(VMCS_GUEST_CR3); break;
            case 4: val = VMM::VMRead(VMCS_GUEST_CR4); break;
            default: break;
        }
        SetGPR(reg, val);
    } else if (access_type == 2) {
        // clts: clear cr0.ts
        uint64_t cr0 = VMM::VMRead(VMCS_GUEST_CR0);
        cr0 &= ~(1ULL << 3); // clear ts bit
        VMM::VMWrite(VMCS_GUEST_CR0, cr0);
        VMM::VMWrite(VMCS_CR0_READ_SHADOW, cr0);
    } else if (access_type == 3) {
        // lmsw: load machine status word (lower 16 bits of cr0)
        uint64_t cr0 = VMM::VMRead(VMCS_GUEST_CR0);
        uint64_t val = GetGPR(reg) & 0xFFFF;
        // lmsw can only set pe, not clear it
        cr0 = (cr0 & 0xFFFFFFFFFFFF0000ULL) | val;
        cr0 |= 0x20; // ne
        VMM::VMWrite(VMCS_GUEST_CR0, cr0);
        VMM::VMWrite(VMCS_CR0_READ_SHADOW, cr0);
    }

    AdvanceGuestRIP(cpu);
    return VMEXIT_HANDLED;
}

//  exceptions & interrupts

VMExitAction VMExitHandler::HandleExceptionNMI(vCPU* cpu) {
    // read interrupt information from vmcs
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

    // re-inject exception back into guest via vm-entry event injection
    // bits [7:0] = vector, [10:8] = type, [11] = error code valid, [31] = valid
    if (cpu->type == VIRT_INTEL_VTX) {
        // triple fault (vector 0, type 3=7 in some encodings) → crash
        if (vector == 8 || (vector == 0 && type == 3)) {
            // #df or triple fault - halt the vm
            SerialLogger::Log("VMExit: Fatal exception - halting VM\r\n");
            return VMEXIT_SHUTDOWN;
        }

        // build injection info: same vector and type, mark valid (bit 31)
        uint32_t inject = (1u << 31) | ((uint32_t)type << 8) | vector;
        if (has_error) {
            inject |= (1u << 11); // error code delivery
            uint32_t err_code = (uint32_t)VMM::VMRead(VMCS_EXIT_INT_ERROR);
            VMM::VMWrite(VMCS_ENTRY_EXCEPTION_ERROR, err_code);
        }
        VMM::VMWrite(VMCS_ENTRY_INT_INFO, inject);

        // for software exceptions/interrupts, set instruction length
        if (type == 4 || type == 6) { // software interrupt/exception
            uint32_t instr_len = (uint32_t)VMM::VMRead(VMCS_EXIT_INSTR_LENGTH);
            VMM::VMWrite(VMCS_ENTRY_INSTR_LENGTH, instr_len);
        }
    }

    // don't advance rip - the guest should re-execute the faulting instruction
    // after handling the exception via its idt
    return VMEXIT_HANDLED;
}

VMExitAction VMExitHandler::HandleExternalInterrupt(vCPU* cpu) {
    (void)cpu;
    // external interrupt (from host) - handle in host, then re-enter guest
    return VMEXIT_CONTINUE;
}

//  ept violation

VMExitAction VMExitHandler::HandleEPTViolation(vCPU* cpu) {
    uint64_t guest_phys = 0;
    uint64_t qualification = cpu->exit_qualification;

    if (cpu->type == VIRT_INTEL_VTX) {
        guest_phys = (uint64_t)VMM::VMRead(VMCS_GUEST_PHYS_ADDR);
    } else if (cpu->type == VIRT_AMD_SVM && cpu->vmcb) {
        guest_phys = cpu->vmcb->exit_info2; // npf stores faulting gpa in exit_info2
        qualification = cpu->vmcb->exit_info1;
    }

    // check if this is an mmio access - route to hypervisor device layer
    bool is_write = (qualification & 0x02) != 0; // bit 1 = write access
    uint32_t mmio_value = 0;
    if (is_write) {
        // for mmio writes, the value is often in eax
        mmio_value = cpu->regs[0];
    }
    if (Hypervisor::HandleGuestMMIO(guest_phys, is_write, 4, mmio_value)) {
        if (!is_write) {
            cpu->regs[0] = mmio_value; // return read value
        }
        AdvanceGuestRIP(cpu);
        return VMEXIT_HANDLED;
    }

    // try ept/npt resolution
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
    SerialLogger::Log(" - shutting down guest\r\n");
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
    SerialLogger::Log(" - FATAL\r\n");
    return VMEXIT_FATAL;
}

//  triple fault - catastrophic guest failure

VMExitAction VMExitHandler::HandleTripleFault(vCPU* cpu) {
    SerialLogger::Log("VMExit: TRIPLE FAULT - guest crashed\r\n");
    VMM::DumpVCPUState(cpu);
    return VMEXIT_REBOOT;
}

//  vmcall - hypercall from guest

VMExitAction VMExitHandler::HandleVMCall(vCPU* cpu) {
    uint32_t call_num = cpu->regs[0]; // eax = hypercall number
    uint32_t arg2 = cpu->regs[1];     // ecx = arg2 / sub-function

    SerialLogger::Log("VMExit: VMCALL #");
    SerialLogger::LogDec(call_num);
    SerialLogger::Log(" sub=");
    SerialLogger::LogHex(arg2);
    SerialLogger::Log("\r\n");

    // hypercall dispatch
    switch (call_num) {
        case 0: // nop / heartbeat
            cpu->regs[0] = 0; // return success
            break;
        case 1: // get hypervisor info
            cpu->regs[0] = 0x4B55524F; // "kuro"
            cpu->regs[3] = 0x4E4F5320; // "no s"
            break;
        case 2: // request shutdown
            return VMEXIT_SHUTDOWN;
        case 3: // request reboot
            return VMEXIT_REBOOT;

        //  0x10 - gpu framebuffer passthrough
        //  ecx(regs[1]) = sub-function
        //    0: get display info
        //       returns: eax=width, ebx=height, ecx=pitch, edx=bpp
        //    1: blit rectangle (guest phys → host framebuffer)
        //       rdi(regs[7]) = guest phys addr of pixel data
        //       esi(regs[6]) = (width << 16) | height
        //       edx(regs[2]) = (dst_x << 16) | dst_y
        //       ebx(regs[3]) = pitch (bytes per row in source)
        //       returns: eax = 0 on success
        //    2: swap / present
        //       returns: eax = 0 on success
        case 0x10: {
            uint32_t subfn = cpu->regs[1]; // ecx
            switch (subfn) {
                case 0: { // get display info
                    LinuxFBInfo* fb = LinuxDeviceBridge::GetFramebufferInfo();
                    cpu->regs[0] = fb ? fb->xres_virtual : (uint64_t)Graphics::GetWidth();
                    cpu->regs[3] = fb ? fb->yres_virtual : (uint64_t)Graphics::GetHeight();
                    cpu->regs[1] = fb ? fb->line_length : (uint64_t)Graphics::GetPitch();
                    cpu->regs[2] = fb ? fb->bits_per_pixel : (uint64_t)Graphics::GetBpp();
                    break;
                }
                case 1: { // blit rectangle from guest memory to host fb
                    uint64_t guest_phys = cpu->regs[7]; // rdi
                    uint32_t wh = (uint32_t)cpu->regs[6]; // rsi
                    uint32_t src_w = wh >> 16;
                    uint32_t src_h = wh & 0xFFFF;
                    uint32_t dst_xy = (uint32_t)cpu->regs[2]; // rdx
                    int dst_x = (int)(dst_xy >> 16);
                    int dst_y = (int)(dst_xy & 0xFFFF);
                    uint32_t src_pitch = (uint32_t)cpu->regs[3]; // ebx

                    uint8_t* src = GuestMemoryManager::GuestPhysToHost(guest_phys);
                    if (!src || src_w == 0 || src_h == 0) {
                        cpu->regs[0] = 0xFFFFFFFF;
                        break;
                    }

                    cpu->regs[0] = LinuxDeviceBridge::BlitFramebufferRect(
                        src, src_pitch, src_w, src_h, dst_x, dst_y) ? 0 : 0xFFFFFFFF;
                    break;
                }
                case 2: { // swap / present
                    LinuxDeviceBridge::PresentFramebuffer();
                    cpu->regs[0] = 0;
                    break;
                }
                default:
                    cpu->regs[0] = 0xFFFFFFFF;
                    break;
            }
            break;
        }

        //  0x11 - audio pcm passthrough
        //  ecx(regs[1]) = sub-function
        //    0: get audio info
        //       returns: eax=sample_rate, ecx=channels, edx=bits_per_sample
        //    1: submit pcm buffer for playback
        //       rdi(regs[7]) = guest phys addr of pcm data
        //       esi(regs[6]) = length in bytes
        //       edx(regs[2]) = (sample_rate << 16) | (bits << 8) | channels
        //       returns: eax = 0 on success
        //    2: stop playback
        //       returns: eax = 0
        //    3: set volume
        //       edx(regs[2]) = volume 0-100
        //       returns: eax = 0
        case 0x11: {
            uint32_t subfn = cpu->regs[1]; // ecx
            switch (subfn) {
                case 0: { // get audio info
                    AudioInfo info = Audio::GetInfo();
                    cpu->regs[0] = (uint32_t)info.sample_rate;
                    cpu->regs[1] = (uint32_t)info.channels;
                    cpu->regs[2] = (uint32_t)info.bits;
                    break;
                }
                case 1: { // submit pcm buffer
                    uint64_t guest_phys = cpu->regs[7]; // rdi
                    uint32_t length = (uint32_t)cpu->regs[6]; // rsi
                    uint32_t fmt = (uint32_t)cpu->regs[2]; // rdx
                    int sample_rate = (int)(fmt >> 16);
                    int bits = (int)((fmt >> 8) & 0xFF);
                    int channels = (int)(fmt & 0xFF);

                    if (sample_rate == 0) sample_rate = 44100;
                    if (bits == 0) bits = 16;
                    if (channels == 0) channels = 2;

                    uint8_t* pcm = GuestMemoryManager::GuestPhysToHost(guest_phys);
                    if (!pcm || length == 0) {
                        cpu->regs[0] = 0xFFFFFFFF;
                        break;
                    }

                    // route guest pcm through the unified audio server so it
                    // mixes with host sound and uses the active backend; map
                    // the guest's bit depth to the pcm sample format.  length
                    // is in bytes. (satoru)
                    AudioFormat::SampleFormat afmt =
                        (bits == 8) ? AudioFormat::FMT_U8 :
                        (bits == 32) ? AudioFormat::FMT_S32_LE :
                                       AudioFormat::FMT_S16_LE;
                    bool ok = AudioServer::PlayPCM(pcm, length, afmt,
                                                   (uint32_t)sample_rate, channels);
                    cpu->regs[0] = ok ? 0 : 0xFFFFFFFF;
                    break;
                }
                case 2: { // stop
                    // stop the active backend via the router (matches the
                    // playpcm reroute above) instead of the legacy sb16 dsp
                    // stop, which would not halt ac97/hda output. (satoru)
                    AudioBackend* be = AudioServer::ActiveBackend();
                    if (be) be->Stop();
                    cpu->regs[0] = 0;
                    break;
                }
                case 3: { // set volume
                    int vol = (int)(cpu->regs[2] & 0xFF); // rdx low byte
                    if (vol > 100) vol = 100;
                    // master volume on the mixer scales every backend's output
                    // through the router. (satoru)
                    AudioMixer::SetMasterVolume(vol);
                    cpu->regs[0] = 0;
                    break;
                }
                default:
                    cpu->regs[0] = 0xFFFFFFFF;
                    break;
            }
            break;
        }

        //  0x12 - wifi / network status passthrough
        //  ecx(regs[1]) = sub-function
        //    0: get network status
        //       returns: eax = state (0=down,1=connected,2=scanning),
        //                ecx = signal_strength_dbm, edx = ip_addr (packed)
        //    1: get ssid
        //       rdi(regs[7]) = guest phys addr of 64-byte output buffer
        //       returns: eax = ssid length (0 if not connected)
        //    2: get mac address
        //       returns: eax = mac[0-3], edx = mac[4-5] (big-endian)
        //    3: get link speed (mbps)
        //       returns: eax = speed in mbps
        case 0x12: {
            uint32_t subfn = cpu->regs[1]; // ecx
            switch (subfn) {
                case 0: { // get network status
                    // query real nic driver for link state
                    // on bare metal with real wifi, alpine guest handles
                    // the actual wpa_supplicant connection. we report the
                    // virtual nic state from kurono's e1000 driver.
                    cpu->regs[0] = 0; // todo: real wifi state from alpine guest
                    cpu->regs[1] = 0; // signal strength
                    cpu->regs[2] = 0; // ip address
                    break;
                }
                case 1: { // get ssid
                    uint64_t guest_buf = cpu->regs[7]; // rdi
                    uint8_t* buf = GuestMemoryManager::GuestPhysToHost(guest_buf);
                    if (buf) {
                        memset(buf, 0, 64);
                        // ssid will be populated by alpine wpa_supplicant query
                    }
                    cpu->regs[0] = 0; // no ssid yet
                    break;
                }
                case 2: { // get mac address
                    cpu->regs[0] = 0; // mac[0-3]
                    cpu->regs[2] = 0; // mac[4-5]
                    break;
                }
                case 3: { // link speed
                    cpu->regs[0] = 0; // mbps
                    break;
                }
                default:
                    cpu->regs[0] = 0xFFFFFFFF;
                    break;
            }
            break;
        }

        //  0x13 - input (keyboard + mouse) passthrough
        //  ecx(regs[1]) = sub-function
        //    0: poll keyboard
        //       returns: eax = ascii char (0 if none), ecx = key enum,
        //                edx = modifier state (bit0=shift,1=ctrl,2=alt)
        //    1: poll mouse
        //       returns: eax = button bitmap (bit0=left,1=right,2=mid),
        //                ecx = abs_x, edx = abs_y
        //    2: check specific key
        //       edx(regs[2]) = key enum value
        //       returns: eax = 1 if pressed, 0 if not
        case 0x13: {
            uint32_t subfn = cpu->regs[1]; // ecx
            switch (subfn) {
                case 0: { // poll keyboard
                    Keyboard::Poll();
                    if (Keyboard::HasChar()) {
                        char c = Keyboard::GetChar();
                        cpu->regs[0] = (uint32_t)(uint8_t)c;
                    } else {
                        cpu->regs[0] = 0;
                    }
                    const KeyboardState& ks = Keyboard::GetState();
                    uint32_t mods = 0;
                    if (ks.shift) mods |= 1;
                    if (ks.ctrl)  mods |= 2;
                    if (ks.alt)   mods |= 4;
                    if (ks.super) mods |= 8;
                    cpu->regs[2] = mods; // edx
                    break;
                }
                case 1: { // poll mouse
                    Mouse::Poll();
                    uint32_t btns = 0;
                    if (Mouse::IsLeftDown())  btns |= 1;
                    if (Mouse::RightClicked()) btns |= 2;
                    cpu->regs[0] = btns;
                    int mx_val, my_val;
                    Mouse::GetPosition(mx_val, my_val);
                    cpu->regs[1] = (uint32_t)mx_val; // ecx = x
                    cpu->regs[2] = (uint32_t)my_val; // edx = y
                    break;
                }
                case 2: { // check specific key
                    Key k = (Key)(cpu->regs[2] & 0xFF); // rdx
                    cpu->regs[0] = Keyboard::IsKeyDown(k) ? 1 : 0;
                    break;
                }
                default:
                    cpu->regs[0] = 0xFFFFFFFF;
                    break;
            }
            break;
        }

        //  0x14 - network packet passthrough
        //  ecx(regs[1]) = sub-function
        //    0: send packet
        //       rdi(regs[7]) = guest phys addr of packet data
        //       esi(regs[6]) = packet length in bytes
        //       returns: eax = 0 on success
        //    1: receive packet (poll)
        //       rdi(regs[7]) = guest phys addr of receive buffer
        //       esi(regs[6]) = buffer max length
        //       returns: eax = bytes received (0 if no packet)
        //    2: get tx/rx stats
        //       returns: eax = tx_packets, ecx = rx_packets,
        //                edx = tx_bytes (low 32), ebx = rx_bytes (low 32)
        case 0x14: {
            uint32_t subfn = cpu->regs[1]; // ecx
            switch (subfn) {
                case 0: { // send packet
                    uint64_t guest_phys = cpu->regs[7]; // rdi
                    uint32_t pkt_len = (uint32_t)cpu->regs[6]; // rsi
                    uint8_t* pkt = GuestMemoryManager::GuestPhysToHost(guest_phys);
                    if (!pkt || pkt_len == 0 || pkt_len > 1518) {
                        cpu->regs[0] = 0xFFFFFFFF;
                        break;
                    }
                    // todo: forward packet to real nic via kurono's e1000 driver
                    // for now, accept and drop (no real nic path yet on bare metal)
                    cpu->regs[0] = 0;
                    break;
                }
                case 1: { // receive packet
                    uint64_t guest_phys = cpu->regs[7]; // rdi
                    uint32_t buf_max = (uint32_t)cpu->regs[6]; // rsi
                    uint8_t* buf = GuestMemoryManager::GuestPhysToHost(guest_phys);
                    if (!buf || buf_max == 0) {
                        cpu->regs[0] = 0;
                        break;
                    }
                    // todo: poll real nic rx ring for incoming packets
                    cpu->regs[0] = 0; // no packet available yet
                    break;
                }
                case 2: { // get stats
                    cpu->regs[0] = 0; // tx_packets
                    cpu->regs[1] = 0; // rx_packets
                    cpu->regs[2] = 0; // tx_bytes
                    cpu->regs[3] = 0; // rx_bytes
                    break;
                }
                default:
                    cpu->regs[0] = 0xFFFFFFFF;
                    break;
            }
            break;
        }

        //  0x20 - 9p shared filesystem (host kvfs ↔ guest)
        //  all sub-functions dispatched by v9fs::handlevmcall.
        //  ecx = v9fs_op sub-function, other regs carry args.
        //  returns: eax = v9fs_err
        case 0x20: {
            V9FS::HandleVMCall(cpu->regs);
            break;
        }

        //  0x4b ('K') - ksa restricted result channel.
        //  this is the ONLY bridge out of the ksa isolated context. it is
        //  READ-ONLY: there is no sub-function that writes an approval into
        //  ksa memory, so the main os cannot forge a "yes". (satoru)
        //  ecx(regs[1]) = sub-function:
        //    KSA_SUB_GET_VERDICT(0): returns eax=completed, ebx=approved,
        //                            ecx=have_cred_hash. the credential hash
        //                            itself is never exposed over the channel - 
        //                            only supr (in-kernel) consumes it. (satoru)
        //    KSA_SUB_GET_INFO(1):    returns eax=channel revision.
        case KSA_VMCALL_CHANNEL: {
            uint32_t subfn = cpu->regs[1]; // ecx
            switch (subfn) {
                case KSA_SUB_GET_VERDICT: {
                    KSAVerdict v;
                    bool ok = KSA::ReadVerdictForChannel(v);
                    cpu->regs[0] = (ok && v.completed) ? 1 : 0; // eax
                    cpu->regs[3] = v.approved ? 1 : 0;          // ebx
                    cpu->regs[1] = v.have_cred_hash ? 1 : 0;    // ecx
                    break;
                }
                case KSA_SUB_GET_INFO:
                    cpu->regs[0] = KSA::ChannelRevision();
                    break;
                default:
                    cpu->regs[0] = 0xFFFFFFFF;
                    break;
            }
            break;
        }

        default:
            cpu->regs[0] = 0xFFFFFFFF; // unknown
            break;
    }

    AdvanceGuestRIP(cpu);
    return VMEXIT_HANDLED;
}

//  rdtsc / rdtscp - timestamp counter access

VMExitAction VMExitHandler::HandleRDTSC(vCPU* cpu) {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    cpu->regs[0] = lo; // eax
    cpu->regs[2] = hi; // edx
    AdvanceGuestRIP(cpu);
    return VMEXIT_HANDLED;
}

VMExitAction VMExitHandler::HandleRDTSCP(vCPU* cpu) {
    // same as rdtsc but also set ecx to ia32_tsc_aux
    HandleRDTSC(cpu);
    cpu->regs[1] = 0; // ecx = tsc_aux (processor id typically)
    return VMEXIT_HANDLED;
}

//  miscellaneous exits

VMExitAction VMExitHandler::HandleINVLPG(vCPU* cpu) {
    // guest invalidated a tlb page - we could invalidate our ept mapping
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
    // vmx preemption timer fired - time slice expired
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
    // write-back and invalidate cache - expensive, just skip
    AdvanceGuestRIP(cpu);
    return VMEXIT_HANDLED;
}

VMExitAction VMExitHandler::HandleXSETBV(vCPU* cpu) {
    // extended control register write - used for avx state
    AdvanceGuestRIP(cpu);
    return VMEXIT_HANDLED;
}

//  guest rip advancement

void VMExitHandler::AdvanceGuestRIP(vCPU* cpu) {
    if (!cpu) return;

    if (cpu->type == VIRT_INTEL_VTX) {
        uint32_t instr_len = VMM::VMRead(VMCS_EXIT_INSTR_LENGTH);
        uint32_t rip = VMM::VMRead(VMCS_GUEST_RIP);
        VMM::VMWrite(VMCS_GUEST_RIP, rip + instr_len);
    } else if (cpu->type == VIRT_AMD_SVM && cpu->vmcb) {
        // svm gives us next_rip directly
        if (cpu->vmcb->next_rip != 0) {
            cpu->vmcb->rip = cpu->vmcb->next_rip;
        } else {
            // fallback: some exits don't set next_rip, advance by 2 (typical for i/o)
            cpu->vmcb->rip += 2;
        }
    }
}

//  cpuid override management

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

//  statistics

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
