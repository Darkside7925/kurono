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
#include "../drivers/cpu_detect.h"
#include "../proc/smp.h"
#include "../kernel/time.h"

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

// features the guest must not see because this vmm cannot back them (satoru):
//  - xsave/osxsave/avx/fma/f16c: xcr0 is not switched and xsetbv is only
//    skipped, so the guest would run avx with the host's xcr0 and #ud
//  - tsc-deadline, x2apic: the lapic is emulated in xapic mmio mode only
//  - leaf 7 invpcid: needs secondary control "enable invpcid" (sdm 25.3),
//    otherwise invpcid is #ud in non-root mode. linux 6.18 hit exactly that
//    in native_flush_tlb_global and panicked in setup_arch
//  - leaf 1 ecx.monitor, leaf 5: with mwait advertised linux idles in mwait and
//    never executes hlt; an mwait that returns at once kept the host cpu at 100%
//    on idle (the idle loop spun through exits). hidden, the guest uses hlt (satoru)
//  - leaf 7 waitpkg: needs "enable user wait and pause"
//  - leaf 1 edx.apic: lapic mmio would need instruction decode, which the
//    ept-violation path does not do (it advanced rip by an undefined
//    instruction length and linux oopsed in native_apic_mem_read)
static void MaskGuestCPUID(uint32_t leaf, uint32_t subleaf,
                           uint32_t& ebx, uint32_t& ecx, uint32_t& edx) {
    if (leaf == 1) {
        if (Hypervisor::GuestCpus() <= 1) edx &= ~(1u << 9);     // smp guest: the lapic is emulated (satoru)
        edx &= ~(1u << 28);                                      // no hyperthreading bit (satoru)
        ecx &= ~(1u << 3);           // monitor/mwait (satoru)
        ecx &= ~((1u << 12) | (1u << 21) | (1u << 24) | (1u << 26) |
                 (1u << 27) | (1u << 28) | (1u << 29));
    } else if (leaf == 7 && subleaf == 0) {
        ebx &= ~(1u << 10);
        ecx &= ~(1u << 5);
    }
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
    // the hypervisor bit stays set: linux only looks at the 0x40000000 leaves
    // (kvm-clock, see below) when it is (satoru)
    ecx |= CPUID_HYPERVISOR_BIT;
    MaskGuestCPUID(1, 0, ebx, ecx, edx); // (satoru)
    AddCPUIDOverride(1, 0, eax, ebx, ecx, edx);

    SerialLogger::Log("VMExit: Handler initialized with default CPUID overrides\r\n");
}

// mov dr exits (proc control bit 23): keep dr0-3/dr6 virtual and route dr7
// to the vmcs guest field, which "load debug controls" puts in place on entry.
// the handler was missing, so linux's first __show_regs / hw_breakpoint init
// made the vm fatal (satoru)
static uint64_t g_guest_dr[8] = { 0, 0, 0, 0, 0, 0, 0xFFFF0FF0ULL, 0x400ULL };
uint64_t* VMExitHandler::GuestDRs() { vCPU* c = VMM::Current(); return c ? c->dr : g_guest_dr; }

static VMExitAction HandleMovDR(vCPU* cpu) {
    uint64_t q = cpu->exit_qualification;
    uint32_t dr = (uint32_t)(q & 7);
    bool from_dr = (q >> 4) & 1;
    uint32_t reg = (uint32_t)((q >> 8) & 0xF);
    if (from_dr) {
        uint64_t v = (dr == 7) ? VMM::VMRead(0x681A) : VMExitHandler::GuestDRs()[dr];
        if (dr == 4) v = VMExitHandler::GuestDRs()[6];
        if (dr == 5) v = VMM::VMRead(0x681A);
        if (reg == 4) VMM::VMWrite(VMCS_GUEST_RSP, v);
        else cpu->regs[reg] = v;
    } else {
        uint64_t v = (reg == 4) ? VMM::VMRead(VMCS_GUEST_RSP) : cpu->regs[reg];
        if (dr == 7 || dr == 5) VMM::VMWrite(0x681A, (v & 0xFFFFFFFFULL) | 0x400ULL);
        else if (dr == 4 || dr == 6) VMExitHandler::GuestDRs()[6] = v;
        else VMExitHandler::GuestDRs()[dr] = v;
    }
    VMExitHandler::AdvanceGuestRIP(cpu);
    return VMEXIT_HANDLED;
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

        case EXIT_REASON_MOV_DR: // (satoru)
            return HandleMovDR(cpu);

        case EXIT_REASON_INT_WINDOW: // guest can take an irq now (satoru)
            VMM::VMWrite(VMCS_PROC_BASED_CONTROLS,
                         VMM::VMRead(VMCS_PROC_BASED_CONTROLS) & ~(uint64_t)PROC_INT_WINDOW_EXIT);
            return VMEXIT_HANDLED;

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
            if (leaf == 1 && Hypervisor::GuestCpus() > 1) cpu->regs[2] |= (1u << 9);   // lapic present (satoru)
            if (leaf == 1) cpu->regs[3] = (cpu->regs[3] & 0x0000FFFF) | ((uint64_t)Hypervisor::CurrentVcpu() << 24) | (1u << 16);   // apic id, 1 logical cpu (satoru)
            AdvanceGuestRIP(cpu);
            return VMEXIT_HANDLED;
        }
    }

    // no override - execute real cpuid and pass through
    uint32_t eax, ebx, ecx, edx;
    asm volatile("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(leaf), "c"(subleaf));
    // the 0x40000000 range belongs to the outer kvm when nested; passing it
    // through advertises kvmclock msrs we do not emulate (satoru)
    if (leaf >= 0x40000000 && leaf <= 0x4FFFFFFF) eax = ebx = ecx = edx = 0;
    // our own kvm leaves: the signature plus kvm-clock (clocksource2 + the
    // stable bit) and nothing else, so the guest gets an exact clock and tsc
    // frequency without calibrating against the emulated pit (satoru)
    if (leaf == 0x40000000) { eax = 0x40000001; ebx = 0x4B4D564B; ecx = 0x564B4D56; edx = 0x4D; }
    else if (leaf == 0x40000001) { eax = (1u << 3) | (1u << 24); ebx = ecx = edx = 0; }
    MaskGuestCPUID(leaf, subleaf, ebx, ecx, edx);
    if (leaf == 5 || leaf == 0xB || leaf == 0x1F) eax = ebx = ecx = edx = 0;   // flat topology from the mp table (satoru)
    if ((leaf == 0xB || leaf == 0x1F) && Hypervisor::GuestCpus() > 1) {
        // smp: the (x2)apic id linux reads in secondary_startup_64 to find its cpu number;
        // one thread per core, all cores in one package (satoru)
        uint32_t id = (uint32_t)Hypervisor::CurrentVcpu(), n = (uint32_t)Hypervisor::GuestCpus();
        uint32_t bits = 0; while ((1u << bits) < n) bits++;
        if (subleaf == 0) { eax = 0; ebx = 1; ecx = 0x100; }
        else if (subleaf == 1) { eax = bits; ebx = n; ecx = 0x201; }
        else { eax = 0; ebx = 0; ecx = subleaf; }
        edx = id;
    }   // no monitor/mwait leaf (satoru)

    // always hide vmx/svm capability from guest
    if (leaf == 1) {
        ecx &= ~CPUID_VMX_BIT;
        ecx &= ~(1u << 3);           // monitor/mwait (satoru)
        ecx |= CPUID_HYPERVISOR_BIT; // (satoru)
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
    // an idle guest halts thousands of times a second; logging each one
    // flooded the host serial log and slowed the guest (satoru)
    if (stats.hlt_exits <= 4) SerialLogger::Log("VMExit: Guest HLT\r\n");
    // advance past the hlt; the run loop sleeps the host thread until an
    // interrupt is pending instead of spinning the cpu (satoru)
    Hypervisor::NoteHalt();
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
        // unhandled port - log it for debugging (the first few: linux probes
        // every legacy port at boot) (satoru)
        static uint32_t s_unh_budget = 64;
        if (s_unh_budget) {
            s_unh_budget--;
            SerialLogger::Log("VMExit: Unhandled I/O port=0x");
            SerialLogger::LogHex(io.port);
            SerialLogger::Log(io.is_out ? " OUT" : " IN");
            SerialLogger::Log(" size=");
            SerialLogger::LogDec(io.size);
            SerialLogger::Log("\r\n");
        }
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

// plain store for msrs the guest writes and reads back (pat, misc_enable,
// mtrrs ...). unknown msrs used to read 0 whatever was written (satoru)
struct GuestMSRSlot { uint32_t msr; bool used; uint64_t val; };
static GuestMSRSlot g_guest_msr_store[48];

static bool GuestMSRStoreGet(uint32_t msr, uint64_t& out) {
    for (auto& s : g_guest_msr_store) {
        if (s.used && s.msr == msr) { out = s.val; return true; }
    }
    return false;
}

static void GuestMSRStoreSet(uint32_t msr, uint64_t val) {
    for (auto& s : g_guest_msr_store) {
        if (s.used && s.msr == msr) { s.val = val; return; }
    }
    for (auto& s : g_guest_msr_store) {
        if (!s.used) { s.used = true; s.msr = msr; s.val = val; return; }
    }
}

// returns true when the msr lives in the vmcs or the vmm swap table (satoru)
static bool GuestMSRRead(uint32_t msr, uint64_t& value) {
    if (msr == 0xC0000080) { value = VMM::VMRead(VMCS_GUEST_IA32_EFER); return true; }
    if (msr == 0xC0000100) { value = VMM::VMRead(VMCS_GUEST_FS_BASE); return true; }
    if (msr == 0xC0000101) { value = VMM::VMRead(VMCS_GUEST_GS_BASE); return true; }
    if (uint64_t* p = VMM::GuestSwitchedMSR(msr)) { value = *p; return true; }
    return false;
}

static bool GuestMSRWrite(uint32_t msr, uint64_t value) {
    if (msr == 0xC0000080) {
        // lma (bit 10) is read-only and tracks cr0.pg && lme (satoru)
        uint64_t cur = VMM::VMRead(VMCS_GUEST_IA32_EFER);
        VMM::VMWrite(VMCS_GUEST_IA32_EFER, (value & ~(1ULL << 10)) | (cur & (1ULL << 10)));
        return true;
    }
    if (msr == 0xC0000100) { VMM::VMWrite(VMCS_GUEST_FS_BASE, value); return true; }
    if (msr == 0xC0000101) { VMM::VMWrite(VMCS_GUEST_GS_BASE, value); return true; }
    if (uint64_t* p = VMM::GuestSwitchedMSR(msr)) { *p = value; return true; }
    return false;
}

//  kvm-clock (pvclock) for the guest (satoru)
//
// the guest reads the host tsc directly (no rdtsc exiting, no offset), so one
// pvclock record written when the guest enables it stays valid: system time =
// (tsc - tsc_timestamp) scaled by mul/shift. the record is rewritten on every
// enable. the wall clock record holds the utc time at guest system time 0.
static uint64_t g_kvmclock_tsc0 = 0;      // tsc at guest system time 0
static uint64_t g_kvmclock_hz = 0;
static uint32_t g_kvmclock_mul = 0;
static int8_t   g_kvmclock_shift = 0;
static uint32_t g_kvmclock_ver = 0;
static uint64_t g_msr_kvm_systime = 0, g_msr_kvm_wall = 0;

// the guest's tsc: the host's plus the offset of the host cpu this runs on, so every vcpu sees one
// timeline although the laptop's cpus disagree on the tsc (satoru)
static int64_t g_tsc_off[SMP_MAX_CPUS];
void VMExitHandler::SetTscOffset(uint32_t cpu, int64_t off) { if (cpu < SMP_MAX_CPUS) g_tsc_off[cpu] = off; }
static inline uint64_t kc_rdtsc() {
    uint32_t lo, hi; asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint32_t ci = SMP::CpuIndex();
    return (((uint64_t)hi << 32) | lo) + (uint64_t)(ci < SMP_MAX_CPUS ? g_tsc_off[ci] : 0);
}

// kvm_get_time_scale(): ns = ((tsc << shift) * mul) >> 32
static void kc_scale(uint64_t scaled_hz, uint64_t base_hz, int8_t* pshift, uint32_t* pmul) {
    uint64_t tps64 = base_hz, scaled64 = scaled_hz;
    int32_t shift = 0;
    while (tps64 > scaled64 * 2 || (tps64 & 0xFFFFFFFF00000000ULL)) { tps64 >>= 1; shift--; }
    uint32_t tps32 = (uint32_t)tps64;
    while (tps32 <= scaled64 || (scaled64 & 0xFFFFFFFF00000000ULL)) {
        if ((scaled64 & 0xFFFFFFFF00000000ULL) || (tps32 & 0x80000000u)) scaled64 >>= 1;
        else tps32 <<= 1;
        shift++;
    }
    *pshift = (int8_t)shift;
    *pmul = (uint32_t)((scaled64 << 32) / tps32);
}

static uint64_t kc_tsc_to_ns(uint64_t delta) {
    if (g_kvmclock_shift < 0) delta >>= -g_kvmclock_shift; else delta <<= g_kvmclock_shift;
    return (uint64_t)(((unsigned __int128)delta * g_kvmclock_mul) >> 32);
}

void VMExitHandler::ResetGuestClock() {
    g_kvmclock_hz = CPUDetect::GetInfo().frequency.tsc_frequency;
    if (g_kvmclock_hz < 1000000ULL) g_kvmclock_hz = 2000000000ULL;
    kc_scale(1000000000ULL, g_kvmclock_hz, &g_kvmclock_shift, &g_kvmclock_mul);
    g_kvmclock_tsc0 = kc_rdtsc();
    g_msr_kvm_systime = g_msr_kvm_wall = 0;
}

uint64_t VMExitHandler::GuestClockNs() {
    if (!g_kvmclock_tsc0) return 0;
    return kc_tsc_to_ns(kc_rdtsc() - g_kvmclock_tsc0);
}

static void kc_write_systime(uint64_t gpa) {
    struct __attribute__((packed)) PV {
        uint32_t version, pad0; uint64_t tsc_timestamp, system_time;
        uint32_t mul; int8_t shift; uint8_t flags; uint8_t pad[2];
    } pv;
    uint8_t* h = GuestMemoryManager::GuestPhysToHost(gpa);
    if (!h || GuestMemoryManager::ContigBytes(gpa) < sizeof(pv)) return;
    volatile PV* g = (volatile PV*)h;
    g_kvmclock_ver += 2;
    g->version = g_kvmclock_ver | 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint64_t t = kc_rdtsc();
    g->tsc_timestamp = t;
    g->system_time = kc_tsc_to_ns(t - g_kvmclock_tsc0);
    g->mul = g_kvmclock_mul;
    g->shift = g_kvmclock_shift;
    g->flags = 1;                            // PVCLOCK_TSC_STABLE_BIT
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    g->version = g_kvmclock_ver + 2;
    g_kvmclock_ver += 2;
    (void)pv;
}

static void kc_write_wall(uint64_t gpa) {
    uint8_t* h = GuestMemoryManager::GuestPhysToHost(gpa);
    if (!h || GuestMemoryManager::ContigBytes(gpa) < 12) return;
    uint64_t now_ns = TimeManager::NowUTC().us * 1000ULL;
    uint64_t up_ns = VMExitHandler::GuestClockNs();
    uint64_t boot_ns = now_ns > up_ns ? now_ns - up_ns : 0;   // a broken rtc must not wrap (satoru)
    volatile uint32_t* w = (volatile uint32_t*)h;
    w[0] = 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    w[1] = (uint32_t)(boot_ns / 1000000000ULL);
    w[2] = (uint32_t)(boot_ns % 1000000000ULL);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    w[0] = 2;
}

static bool KvmClockMSRWrite(uint32_t msr, uint64_t value) {
    if (msr == 0x4B564D01) {                 // MSR_KVM_SYSTEM_TIME_NEW
        g_msr_kvm_systime = value;
        if (value & 1) kc_write_systime(value & ~0x1FULL);
        return true;
    }
    if (msr == 0x4B564D00) {                 // MSR_KVM_WALL_CLOCK_NEW
        g_msr_kvm_wall = value;
        kc_write_wall(value);
        return true;
    }
    return false;
}

static bool KvmClockMSRRead(uint32_t msr, uint64_t& value) {
    if (msr == 0x4B564D01) { value = g_msr_kvm_systime; return true; }
    if (msr == 0x4B564D00) { value = g_msr_kvm_wall; return true; }
    return false;
}

// rdmsr/wrmsr/cr exits are routine for a running linux; log only the first
// few of each so the host serial log stays usable (satoru)
static uint32_t g_msr_log_budget = 96, g_cr_log_budget = 48;
void VMExitHandler::ResetLogBudget() { g_msr_log_budget = 96; g_cr_log_budget = 48; }

VMExitAction VMExitHandler::HandleMSRRead(vCPU* cpu) {
    uint32_t msr = cpu->regs[1]; // ecx = msr address

    if (g_msr_log_budget) {
        g_msr_log_budget--;
        SerialLogger::Log("VMExit: RDMSR ");
        SerialLogger::LogHex(msr);
        SerialLogger::Log("\r\n");
    }

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
            value = (Hypervisor::CurrentVcpu() == 0) ? 0xFEE00900 : 0xFEE00800; // default apic base, bsp only on cpu 0
            break;
        case 0xC0000080: // ia32_efer
            value = 0; // guest in 32-bit mode, no long mode
            break;
        case 0x174: // ia32_sysenter_cs
        case 0x175: // ia32_sysenter_esp
        case 0x176: // ia32_sysenter_eip
            value = 0; // not configured
            break;
        case 0x1A0: // ia32_misc_enable: fast strings on, bts/pebs unavailable.
                    // 0 made linux turn rep movs/stos fast strings off (satoru)
            value = (1ULL << 0) | (1ULL << 11) | (1ULL << 12);
            break;
        case 0x277: // ia32_pat power-on default (satoru)
            value = 0x0007040600070406ULL;
            break;
        default:
            // return 0 for unknown msrs rather than #gp
            value = 0;
            break;
    }
    GuestMSRRead(msr, value);
    GuestMSRStoreGet(msr, value);
    KvmClockMSRRead(msr, value); // (satoru)

    cpu->regs[0] = (uint32_t)(value & 0xFFFFFFFF);  // eax = low 32
    cpu->regs[2] = (uint32_t)(value >> 32);           // edx = high 32

    AdvanceGuestRIP(cpu);
    return VMEXIT_HANDLED;
}

VMExitAction VMExitHandler::HandleMSRWrite(vCPU* cpu) {
    uint32_t msr = cpu->regs[1]; // ecx
    uint64_t value = ((uint64_t)cpu->regs[2] << 32) | cpu->regs[0]; // edx:eax

    if (g_msr_log_budget) {
        g_msr_log_budget--;
        SerialLogger::Log("VMExit: WRMSR ");
        SerialLogger::LogHex(msr);
        SerialLogger::Log(" = ");
        SerialLogger::LogHex((uint32_t)(value >> 32));
        SerialLogger::LogHex((uint32_t)value);
        SerialLogger::Log("\r\n");
    }

    // efer, fs/gs base and the syscall msrs must reach the real guest state;
    // everything else is remembered so a read returns it (satoru)
    if (!KvmClockMSRWrite(msr, value) && !GuestMSRWrite(msr, value)) GuestMSRStoreSet(msr, value);

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

    if (g_cr_log_budget) {
        g_cr_log_budget--;
        SerialLogger::Log("VMExit: CR");
        SerialLogger::LogDec(cr_num);
        SerialLogger::Log(access_type == 0 ? " write" : " read");
        SerialLogger::Log(" reg=");
        SerialLogger::LogDec(reg);
        SerialLogger::Log("\r\n");
    }

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
                VMM::VMWrite(VMCS_CR0_READ_SHADOW, val);
                val |= 0x20;  // ne must be set
                VMM::VMWrite(VMCS_GUEST_CR0, val);
                // a trapped mov cr0 is not executed by the cpu, so the
                // efer.lma / ia-32e entry control update of sdm 26.3.2.1 has
                // to be done here when pg turns on with lme set (satoru)
                {
                    uint64_t efer = VMM::VMRead(VMCS_GUEST_IA32_EFER);
                    uint64_t ent = VMM::VMRead(VMCS_ENTRY_CONTROLS);
                    if ((val & (1ULL << 31)) && (efer & (1ULL << 8))) {
                        efer |= (1ULL << 10);
                        ent |= VM_ENTRY_IA32E_MODE;
                    } else if (!(val & (1ULL << 31))) {
                        efer &= ~(1ULL << 10);
                        ent &= ~(uint64_t)VM_ENTRY_IA32E_MODE;
                    }
                    VMM::VMWrite(VMCS_GUEST_IA32_EFER, efer);
                    VMM::VMWrite(VMCS_ENTRY_CONTROLS, ent);
                }
                break;
            }
            case 3:
                // cr3 write: page table base change - update and flush guest tlb.
                // with cr4.pcide linux sets bit 63 (no-flush); a real mov cr3
                // never stores it, and a guest cr3 with bits above maxphyaddr
                // fails the sdm 26.3.1.1 check on the next entry (satoru)
                VMM::VMWrite(VMCS_GUEST_CR3, val & ~(1ULL << 63));
                break;
            case 4:
                // cr4 write: update and shadow. keep the vmx fixed-1 bits
                // (vmxe) in the real cr4 or the next entry fails (satoru)
                {
                    uint64_t f0 = 0;
                    VMM::SafeReadMSR(MSR_IA32_VMX_CR4_FIXED0, f0);
                    VMM::VMWrite(VMCS_GUEST_CR4, val | f0);
                }
                VMM::VMWrite(VMCS_CR4_READ_SHADOW, val);
                break;
            default:
                break;
        }
    } else if (access_type == 1) {
        // mov from crn - guest is reading
        uint64_t val = 0;
        switch (cr_num) {
            case 0: val = VMM::VMRead(VMCS_CR0_READ_SHADOW); break; // (satoru)
            case 3: val = VMM::VMRead(VMCS_GUEST_CR3); break;
            case 4: val = VMM::VMRead(VMCS_CR4_READ_SHADOW); break; // (satoru)
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

//  mmio instruction emulation (satoru)
//
// an ept violation does not report the instruction length or the register
// (sdm 28.2.1: the vm-exit instruction-length field is only defined for
// instruction exits), so the old path advanced rip by a stale length and
// moved the value through eax whatever the instruction was. linux's
// ioread8 / readl / writel / apic reads then resumed mid-instruction.
// fetch the instruction through the guest page tables and decode the mov
// forms mmio accessors compile to.

// guest linear -> guest physical with the guest's own paging (4-level, pae
// and 32-bit paging; 1g/2m/4m pages). returns false if not present (satoru)
static bool GuestLinearToPhys(uint64_t va, uint64_t& pa) {
    uint64_t cr0 = VMM::VMRead(VMCS_GUEST_CR0);
    if (!(cr0 & (1ULL << 31))) { pa = va & 0xFFFFFFFFULL; return true; }
    uint64_t cr3 = VMM::VMRead(VMCS_GUEST_CR3);
    uint64_t cr4 = VMM::VMRead(VMCS_GUEST_CR4);
    uint64_t efer = VMM::VMRead(VMCS_GUEST_IA32_EFER);
    const uint64_t AM = 0x000FFFFFFFFFF000ULL;
    if (efer & (1ULL << 10)) {
        uint64_t tbl = cr3 & AM;
        for (int lvl = 3; lvl >= 0; lvl--) {
            uint64_t idx = (va >> (12 + 9 * lvl)) & 0x1FF, e = 0;
            if (!GuestMemoryManager::ReadGuestPhys(tbl + idx * 8, &e, 8)) return false;
            if (!(e & 1)) return false;
            if ((lvl == 2 || lvl == 1) && (e & 0x80)) {
                uint64_t span = 1ULL << (12 + 9 * lvl);
                pa = (e & AM & ~(span - 1)) | (va & (span - 1));
                return true;
            }
            if (lvl == 0) { pa = (e & AM) | (va & 0xFFF); return true; }
            tbl = e & AM;
        }
        return false;
    }
    if (cr4 & (1ULL << 5)) {
        uint64_t pdpte = 0;
        if (!GuestMemoryManager::ReadGuestPhys((cr3 & 0xFFFFFFE0ULL) + ((va >> 30) & 3) * 8, &pdpte, 8)) return false;
        if (!(pdpte & 1)) return false;
        uint64_t pde = 0;
        if (!GuestMemoryManager::ReadGuestPhys((pdpte & AM) + ((va >> 21) & 0x1FF) * 8, &pde, 8)) return false;
        if (!(pde & 1)) return false;
        if (pde & 0x80) { pa = (pde & AM & ~0x1FFFFFULL) | (va & 0x1FFFFF); return true; }
        uint64_t pte = 0;
        if (!GuestMemoryManager::ReadGuestPhys((pde & AM) + ((va >> 12) & 0x1FF) * 8, &pte, 8)) return false;
        if (!(pte & 1)) return false;
        pa = (pte & AM) | (va & 0xFFF);
        return true;
    }
    uint32_t pde = 0;
    if (!GuestMemoryManager::ReadGuestPhys((cr3 & 0xFFFFF000ULL) + ((va >> 22) & 0x3FF) * 4, &pde, 4)) return false;
    if (!(pde & 1)) return false;
    if ((pde & 0x80) && (cr4 & (1ULL << 4))) { pa = (pde & 0xFFC00000ULL) | (va & 0x3FFFFF); return true; }
    uint32_t pte = 0;
    if (!GuestMemoryManager::ReadGuestPhys((pde & 0xFFFFF000ULL) + ((va >> 12) & 0x3FF) * 4, &pte, 4)) return false;
    if (!(pte & 1)) return false;
    pa = (pte & 0xFFFFF000ULL) | (va & 0xFFF);
    return true;
}

static bool FetchGuestInstr(uint8_t* buf, int n) {
    uint64_t rip = VMM::VMRead(VMCS_GUEST_RIP);
    uint64_t cs_base = VMM::VMRead(VMCS_GUEST_CS_BASE);
    uint64_t efer = VMM::VMRead(VMCS_GUEST_IA32_EFER);
    uint64_t lin = (efer & (1ULL << 10)) ? rip : ((cs_base + rip) & 0xFFFFFFFFULL);
    for (int i = 0; i < n; i++) {
        uint64_t pa;
        if (!GuestLinearToPhys(lin + i, pa)) return i > 0;
        if (!GuestMemoryManager::ReadGuestPhys(pa, buf + i, 1)) return i > 0;
    }
    return true;
}

static uint64_t* GprSlot(vCPU* cpu, int r, uint64_t& rsp_tmp) {
    if (r == 4) { rsp_tmp = VMM::VMRead(VMCS_GUEST_RSP); return &rsp_tmp; }
    return &cpu->regs[r & 15];
}

// returns true when the instruction was decoded and emulated (satoru)
static bool EmulateMMIO(vCPU* cpu, uint64_t gpa) {
    uint8_t ib[16] = {0};
    if (!FetchGuestInstr(ib, 15)) return false;
    bool long_mode = (VMM::VMRead(VMCS_GUEST_IA32_EFER) >> 10) & 1;
    bool cs_d = (VMM::VMRead(VMCS_GUEST_CS_AR) >> 14) & 1;
    int p = 0, rex = 0;
    bool opsz = false;
    for (;;) {
        uint8_t b = ib[p];
        if (b == 0x66) { opsz = true; p++; continue; }
        if (b == 0x67 || b == 0x2E || b == 0x3E || b == 0x26 || b == 0x36 ||
            b == 0x64 || b == 0x65 || b == 0xF0) { p++; continue; }
        break;
    }
    if (long_mode && (ib[p] & 0xF0) == 0x40) rex = ib[p++];
    bool rexw = rex & 8;
    int osz = rexw ? 8 : ((long_mode || cs_d) ? (opsz ? 2 : 4) : (opsz ? 4 : 2));
    int op = ib[p++], op2 = -1;
    if (op == 0x0F) op2 = ib[p++];

    // mov al/eax/rax <-> [moffs] (A0..A3): no modrm, the absolute address
    // follows (8 bytes in long mode). a Kurono guest's TPM probe reads the TIS
    // registers this way and the decode failure used to kill the vm. (satoru)
    if (op >= 0xA0 && op <= 0xA3) {
        bool a67 = false;
        for (int q = 0; q < p; q++) if (ib[q] == 0x67) a67 = true;
        int alen = long_mode ? (a67 ? 4 : 8) : ((cs_d != a67) ? 4 : 2);
        int msz = (op & 1) ? osz : 1;
        p += alen;
        if (p > 15) return false;
        uint64_t* ax = &cpu->regs[0];
        if (op >= 0xA2) {
            uint64_t v = *ax;
            for (int off = 0; off < msz; off += 4) {
                int chunk = (msz - off) >= 4 ? 4 : (msz - off);
                uint32_t w = (uint32_t)(v >> (8 * off));
                if (chunk < 4) w &= (1u << (8 * chunk)) - 1;
                Hypervisor::HandleGuestMMIO(gpa + off, true, (uint8_t)chunk, w);
            }
        } else {
            uint64_t v = 0;
            for (int off = 0; off < msz; off += 4) {
                int chunk = (msz - off) >= 4 ? 4 : (msz - off);
                uint32_t w = 0;
                if (!Hypervisor::HandleGuestMMIO(gpa + off, false, (uint8_t)chunk, w)) w = 0xFFFFFFFFu;
                if (chunk < 4) w &= (1u << (8 * chunk)) - 1;
                v |= (uint64_t)w << (8 * off);
            }
            if (msz == 1) *ax = (*ax & ~0xFFULL) | (v & 0xFF);
            else if (msz == 2) *ax = (*ax & ~0xFFFFULL) | (v & 0xFFFF);
            else if (msz == 4) *ax = v & 0xFFFFFFFFULL;
            else *ax = v;
        }
        VMM::VMWrite(VMCS_GUEST_RIP, VMM::VMRead(VMCS_GUEST_RIP) + (uint64_t)p);
        return true;
    }

    int size = 0;            // memory access size
    bool is_write = false, has_modrm = true, zext = false;
    int imm_len = 0;
    if (op == 0x88) { size = 1; is_write = true; }
    else if (op == 0x89) { size = osz; is_write = true; }
    else if (op == 0x8A) { size = 1; }
    else if (op == 0x8B) { size = osz; }
    else if (op == 0xC6) { size = 1; is_write = true; imm_len = 1; }
    else if (op == 0xC7) { size = osz; is_write = true; imm_len = (osz == 2) ? 2 : 4; }
    else if (op == 0x0F && (op2 == 0xB6 || op2 == 0xB7)) { size = (op2 == 0xB6) ? 1 : 2; zext = true; }
    else return false;
    (void)has_modrm;

    uint8_t modrm = ib[p++];
    int mod = modrm >> 6, reg = ((modrm >> 3) & 7) | ((rex & 4) ? 8 : 0), rm = modrm & 7;
    if (mod == 3) return false;
    bool a32 = long_mode || cs_d;
    if (a32) {
        if (rm == 4) { uint8_t sib = ib[p++]; if (mod == 0 && (sib & 7) == 5) p += 4; }
        if (mod == 0 && rm == 5) p += 4;
        else if (mod == 1) p += 1;
        else if (mod == 2) p += 4;
    } else {
        if (mod == 0 && rm == 6) p += 2;
        else if (mod == 1) p += 1;
        else if (mod == 2) p += 2;
    }
    uint64_t imm = 0;
    for (int i = 0; i < imm_len; i++) imm |= (uint64_t)ib[p + i] << (8 * i);
    if (imm_len == 4 && osz == 8 && (imm & 0x80000000ULL)) imm |= 0xFFFFFFFF00000000ULL;
    p += imm_len;
    if (p > 15) return false;

    uint64_t rsp_tmp = 0;
    // 8-bit register operand without rex: 4..7 = ah, ch, dh, bh
    bool high8 = (size == 1 && !rex && (reg & 7) >= 4 && (op == 0x88 || op == 0x8A));
    uint64_t* slot = GprSlot(cpu, high8 ? (reg & 3) : reg, rsp_tmp);

    if (is_write) {
        uint64_t v;
        if (imm_len) v = imm;
        else v = high8 ? ((*slot >> 8) & 0xFF) : *slot;
        for (int off = 0; off < size; off += 4) {
            int chunk = (size - off) >= 4 ? 4 : (size - off);
            uint32_t w = (uint32_t)(v >> (8 * off));
            if (chunk < 4) w &= (1u << (8 * chunk)) - 1;
            if (!Hypervisor::HandleGuestMMIO(gpa + off, true, (uint8_t)chunk, w)) {
                // no device behind this address: drop the write (satoru)
            }
        }
    } else {
        uint64_t v = 0;
        for (int off = 0; off < size; off += 4) {
            int chunk = (size - off) >= 4 ? 4 : (size - off);
            uint32_t w = 0;
            if (!Hypervisor::HandleGuestMMIO(gpa + off, false, (uint8_t)chunk, w))
                w = 0xFFFFFFFFu; // open bus reads as all ones (satoru)
            if (chunk < 4) w &= (1u << (8 * chunk)) - 1;
            v |= (uint64_t)w << (8 * off);
        }
        if (zext) {
            int dsz = osz;
            uint64_t cur = *slot;
            if (dsz == 2) *slot = (cur & ~0xFFFFULL) | (v & 0xFFFF);
            else *slot = v; // 32/64-bit destination zero-extends
        } else if (size == 1) {
            if (high8) *slot = (*slot & ~0xFF00ULL) | ((v & 0xFF) << 8);
            else *slot = (*slot & ~0xFFULL) | (v & 0xFF);
        } else if (size == 2) {
            *slot = (*slot & ~0xFFFFULL) | (v & 0xFFFF);
        } else if (size == 4) {
            *slot = v & 0xFFFFFFFFULL;
        } else {
            *slot = v;
        }
        if (reg == 4 && !high8) VMM::VMWrite(VMCS_GUEST_RSP, rsp_tmp);
    }
    VMM::VMWrite(VMCS_GUEST_RIP, VMM::VMRead(VMCS_GUEST_RIP) + (uint64_t)p);
    return true;
}

static bool IsEmulatedMMIO(uint64_t gpa) {
    // vpci bars, rom bars, lapic, ioapic, hpet: everything between the end of
    // guest ram and 4 gb, which the ept leaves unmapped on purpose (satoru)
    uint64_t ram_end = GUEST_HIGH_RAM_START + GuestMemoryManager::GetHighRAMSize();
    return gpa >= ram_end && gpa < 0x100000000ULL;
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

    // lazy guest ram: first touch of a chunk (satoru)
    if (guest_phys >= GUEST_HIGH_RAM_START &&
        guest_phys < GUEST_HIGH_RAM_START + GuestMemoryManager::GetHighRAMSize() &&
        Hypervisor::LazyMapRAM(guest_phys))
        return VMEXIT_HANDLED;

    if (cpu->type == VIRT_INTEL_VTX && IsEmulatedMMIO(guest_phys)) {
        if (EmulateMMIO(cpu, guest_phys)) return VMEXIT_HANDLED;
        uint8_t ib[16] = {0};
        FetchGuestInstr(ib, 15);
        SerialLogger::Log("VMExit: MMIO decode failed at GPA=0x");
        SerialLogger::LogHex64(guest_phys);
        SerialLogger::Log(" bytes:");
        for (int i = 0; i < 8; i++) { SerialLogger::Log(" "); SerialLogger::LogHex(ib[i]); }
        SerialLogger::Log("\r\n");
        return VMEXIT_FATAL;
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

    // v9fs (0x20) is a bulk path, one call per 4 KB: not logged (satoru)
    if (call_num != 0x20) {
        SerialLogger::Log("VMExit: VMCALL #");
        SerialLogger::LogDec(call_num);
        SerialLogger::Log(" sub=");
        SerialLogger::LogHex(arg2);
        SerialLogger::Log("\r\n");
    }

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
        //                            only ken (in-kernel) consumes it. (satoru)
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
    // a guest that mwaits anyway idles like a hlt: the host thread sleeps until an
    // interrupt is pending instead of looping through exits (satoru)
    Hypervisor::NoteHalt();
    AdvanceGuestRIP(cpu);
    return VMEXIT_HANDLED;
}

VMExitAction VMExitHandler::HandlePause(vCPU* cpu) {
    Hypervisor::NoteYield();      // smp: end this vcpu's turn (satoru)
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
        // rip is natural width: a uint32_t here cut 0xffffffff8128bb65 to
        // 0x8128bb67 after the first exit in the 64-bit kernel (satoru)
        uint64_t rip = VMM::VMRead(VMCS_GUEST_RIP);
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
