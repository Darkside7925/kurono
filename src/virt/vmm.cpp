// ═══════════════════════════════════════════════════════════════════════════
//  Kurono OS — Virtual Machine Monitor Implementation
//  Intel VT-x / AMD-V detection, VMCS/VMCB setup, VMX lifecycle
// ═══════════════════════════════════════════════════════════════════════════
#include "vmm.h"
#include "../drivers/serial.h"
#include "../kernel/heap.h"

// VM-exit return stub — VMCS HOST_RIP points here after a VM-exit
// This is a no-op label, execution returns to after VMLAUNCH/VMRESUME inline asm
extern "C" void _vmm_host_return_stub() {
    // On VM-exit, control transfers here; we simply return to the caller
    // (RunVCPU continues after the inline asm block)
}

// Helper: allocate page-aligned memory from kernel heap
static void* AllocAligned(size_t size, size_t align) {
    // Allocate extra for alignment
    void* raw = KernelHeap::Alloc(size + align);
    if (!raw) return nullptr;
    uintptr_t addr = (uintptr_t)raw;
    uintptr_t aligned = (addr + align - 1) & ~(align - 1);
    return (void*)aligned;
}

// ── Static members ──
VirtType VMM::virt_type       = VIRT_NONE;
bool     VMM::initialized     = false;
bool     VMM::vmx_on          = false;
bool     VMM::svm_enabled     = false;
int      VMM::vcpu_count      = 0;
uint32_t VMM::vmx_revision_id = 0;
char     VMM::vendor_string[16] = {0};

// ═══════════════════════════════════════════════════════════════════════════
//  Low-level CPU intrinsics — inline asm for x86_64
// ═══════════════════════════════════════════════════════════════════════════

static inline void cpuid(uint32_t leaf, uint32_t& eax, uint32_t& ebx,
                          uint32_t& ecx, uint32_t& edx) {
    asm volatile("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(leaf), "c"(0));
}

uint64_t VMM::ReadMSR(uint32_t msr) {
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

void VMM::WriteMSR(uint32_t msr, uint64_t value) {
    uint32_t lo = (uint32_t)(value & 0xFFFFFFFF);
    uint32_t hi = (uint32_t)(value >> 32);
    asm volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

uint64_t VMM::ReadCR0() {
    uint64_t v;
    asm volatile("mov %%cr0, %0" : "=r"(v));
    return v;
}

uint64_t VMM::ReadCR3() {
    uint64_t v;
    asm volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

uint64_t VMM::ReadCR4() {
    uint64_t v;
    asm volatile("mov %%cr4, %0" : "=r"(v));
    return v;
}

void VMM::WriteCR4(uint64_t val) {
    asm volatile("mov %0, %%cr4" : : "r"(val));
}

// ═══════════════════════════════════════════════════════════════════════════
//  Initialization & Detection
// ═══════════════════════════════════════════════════════════════════════════

void VMM::Init() {
    if (initialized) return;
    initialized = true;
    vcpu_count = 0;
    vmx_on = false;
    svm_enabled = false;

    SerialLogger::Log("VMM: Initializing...\r\n");

    // Get CPU vendor string
    uint32_t eax, ebx, ecx, edx;
    cpuid(0, eax, ebx, ecx, edx);
    *(uint32_t*)&vendor_string[0] = ebx;
    *(uint32_t*)&vendor_string[4] = edx;
    *(uint32_t*)&vendor_string[8] = ecx;
    vendor_string[12] = 0;

    SerialLogger::Log("VMM: CPU Vendor: ");
    SerialLogger::Log(vendor_string);
    SerialLogger::Log("\r\n");

    // Check for hypervisor (we might be nested)
    cpuid(1, eax, ebx, ecx, edx);
    if (ecx & CPUID_HYPERVISOR_BIT) {
        SerialLogger::Log("VMM: Running under a hypervisor (nested virt)\r\n");
    }

    // Detect VT-x or SVM
    if (DetectVTx()) {
        virt_type = VIRT_INTEL_VTX;
        SerialLogger::Log("VMM: Intel VT-x detected\r\n");

        // Read VMX revision ID from MSR_IA32_VMX_BASIC
        if (IsVTxEnabled()) {
            uint64_t vmx_basic = ReadMSR(MSR_IA32_VMX_BASIC);
            vmx_revision_id = (uint32_t)(vmx_basic & 0x7FFFFFFF);
            SerialLogger::Log("VMM: VMX Revision ID: ");
            SerialLogger::LogHex(vmx_revision_id);
            SerialLogger::Log("\r\n");

            // Log supported EPT capabilities
            uint64_t ept_vpid = ReadMSR(MSR_IA32_VMX_EPT_VPID_CAP);
            SerialLogger::Log("VMM: EPT/VPID capabilities: ");
            SerialLogger::LogHex((uint32_t)(ept_vpid >> 32));
            SerialLogger::LogHex((uint32_t)(ept_vpid & 0xFFFFFFFF));
            SerialLogger::Log("\r\n");
        } else {
            SerialLogger::Log("VMM: VT-x detected but NOT enabled in BIOS\r\n");
        }
    } else if (DetectSVM()) {
        virt_type = VIRT_AMD_SVM;
        SerialLogger::Log("VMM: AMD-V (SVM) detected\r\n");

        if (IsSVMEnabled()) {
            SerialLogger::Log("VMM: SVM is enabled\r\n");
        } else {
            SerialLogger::Log("VMM: SVM detected but disabled in BIOS (VM_CR lock)\r\n");
        }
    } else {
        virt_type = VIRT_NONE;
        SerialLogger::Log("VMM: No hardware virtualization support detected\r\n");
    }

    SerialLogger::Log("VMM: Initialization complete\r\n");
}

bool VMM::IsSupported() { return virt_type != VIRT_NONE; }
VirtType VMM::GetType() { return virt_type; }
const char* VMM::GetVendor() { return vendor_string; }

// ═══════════════════════════════════════════════════════════════════════════
//  Feature Detection
// ═══════════════════════════════════════════════════════════════════════════

bool VMM::DetectVTx() {
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, eax, ebx, ecx, edx);
    return (ecx & CPUID_VMX_BIT) != 0;
}

bool VMM::DetectSVM() {
    uint32_t eax, ebx, ecx, edx;
    // Check AMD extended features
    cpuid(0x80000001, eax, ebx, ecx, edx);
    return (ecx & CPUID_SVM_BIT) != 0;
}

bool VMM::IsVTxEnabled() {
    if (!DetectVTx()) return false;
    uint64_t feature_control = ReadMSR(MSR_IA32_FEATURE_CONTROL);
    // Must be locked and VMXON enabled outside SMX
    if (!(feature_control & FEATURE_CONTROL_LOCKED)) {
        // Not locked — we could enable it by writing the MSR
        return true;  // Potentially available
    }
    return (feature_control & FEATURE_CONTROL_VMXON) != 0;
}

bool VMM::IsSVMEnabled() {
    if (!DetectSVM()) return false;
    uint64_t vm_cr = ReadMSR(MSR_VM_CR);
    // Bit 4 = SVMDIS (SVM disabled when set)
    return (vm_cr & (1 << 4)) == 0;
}

uint32_t VMM::GetVMXRevisionId() { return vmx_revision_id; }

// ═══════════════════════════════════════════════════════════════════════════
//  Intel VMX Operations
// ═══════════════════════════════════════════════════════════════════════════

bool VMM::VMXOn() {
    if (virt_type != VIRT_INTEL_VTX) return false;
    if (vmx_on) return true;

    SerialLogger::Log("VMM: Entering VMX root mode...\r\n");

    // Enable VMX in CR4
    uint32_t cr4 = ReadCR4();
    cr4 |= CR4_VMXE;
    WriteCR4(cr4);

    // Ensure feature control MSR allows VMXON
    uint64_t fc = ReadMSR(MSR_IA32_FEATURE_CONTROL);
    if (!(fc & FEATURE_CONTROL_LOCKED)) {
        fc |= FEATURE_CONTROL_LOCKED | FEATURE_CONTROL_VMXON;
        WriteMSR(MSR_IA32_FEATURE_CONTROL, fc);
    }

    // Allocate VMXON region (4KB aligned)
    VMCSRegion* vmxon_region = (VMCSRegion*)AllocAligned(4096, 4096);
    if (!vmxon_region) {
        SerialLogger::Log("VMM: Failed to allocate VMXON region\r\n");
        return false;
    }

    // Clear and set revision ID
    for (int i = 0; i < 4096; i++) ((uint8_t*)vmxon_region)[i] = 0;
    vmxon_region->revision_id = vmx_revision_id;

    // VMXON instruction
    uint64_t phys_addr = (uint64_t)(uintptr_t)vmxon_region;
    uint8_t  error = 0;
    asm volatile(
        "vmxon %[addr]\n\t"
        "setna %[err]"
        : [err] "=rm"(error)
        : [addr] "m"(phys_addr)
        : "cc", "memory"
    );

    if (error) {
        SerialLogger::Log("VMM: VMXON failed\r\n");
        KernelHeap::Free(vmxon_region);
        return false;
    }

    vmx_on = true;
    SerialLogger::Log("VMM: VMX root mode active\r\n");
    return true;
}

void VMM::VMXOff() {
    if (!vmx_on) return;
    asm volatile("vmxoff" ::: "cc");
    vmx_on = false;

    // Clear VMXE in CR4
    uint32_t cr4 = ReadCR4();
    cr4 &= ~CR4_VMXE;
    WriteCR4(cr4);

    SerialLogger::Log("VMM: VMX root mode deactivated\r\n");
}

bool VMM::VMClear(VMCSRegion* region) {
    if (!vmx_on || !region) return false;
    uint64_t phys = (uint64_t)(uintptr_t)region;
    uint8_t error = 0;
    asm volatile(
        "vmclear %[addr]\n\t"
        "setna %[err]"
        : [err] "=rm"(error)
        : [addr] "m"(phys)
        : "cc", "memory"
    );
    return !error;
}

bool VMM::VMPtrLoad(VMCSRegion* region) {
    if (!vmx_on || !region) return false;
    uint64_t phys = (uint64_t)(uintptr_t)region;
    uint8_t error = 0;
    asm volatile(
        "vmptrld %[addr]\n\t"
        "setna %[err]"
        : [err] "=rm"(error)
        : [addr] "m"(phys)
        : "cc", "memory"
    );
    return !error;
}

void VMM::VMWrite(uint32_t field, uint64_t value) {
    uint64_t f64 = field;  // VMWRITE requires natural-width operands in 64-bit mode
    asm volatile("vmwrite %1, %0" : : "r"(f64), "rm"(value) : "cc");
}

uint64_t VMM::VMRead(uint32_t field) {
    uint64_t value = 0;
    uint64_t f64 = field;  // VMREAD requires natural-width operands in 64-bit mode
    asm volatile("vmread %1, %0" : "=rm"(value) : "r"(f64) : "cc");
    return value;
}

bool VMM::VMLaunch() {
    uint8_t error = 0;
    asm volatile(
        "push %%rbx\n\t"
        "push %%rcx\n\t"
        "push %%rdx\n\t"
        "push %%rsi\n\t"
        "push %%rdi\n\t"
        "push %%rbp\n\t"
        "push %%r8\n\t"
        "push %%r9\n\t"
        "push %%r10\n\t"
        "push %%r11\n\t"
        "push %%r12\n\t"
        "push %%r13\n\t"
        "push %%r14\n\t"
        "push %%r15\n\t"
        "vmlaunch\n\t"
        "setna %[err]\n\t"
        "pop %%r15\n\t"
        "pop %%r14\n\t"
        "pop %%r13\n\t"
        "pop %%r12\n\t"
        "pop %%r11\n\t"
        "pop %%r10\n\t"
        "pop %%r9\n\t"
        "pop %%r8\n\t"
        "pop %%rbp\n\t"
        "pop %%rdi\n\t"
        "pop %%rsi\n\t"
        "pop %%rdx\n\t"
        "pop %%rcx\n\t"
        "pop %%rbx"
        : [err] "=rm"(error)
        :
        : "cc", "memory"
    );
    // If we get here, VMLAUNCH failed (success = control transferred to guest)
    return !error;
}

bool VMM::VMResume() {
    uint8_t error = 0;
    asm volatile(
        "push %%rbx\n\t"
        "push %%rcx\n\t"
        "push %%rdx\n\t"
        "push %%rsi\n\t"
        "push %%rdi\n\t"
        "push %%rbp\n\t"
        "push %%r8\n\t"
        "push %%r9\n\t"
        "push %%r10\n\t"
        "push %%r11\n\t"
        "push %%r12\n\t"
        "push %%r13\n\t"
        "push %%r14\n\t"
        "push %%r15\n\t"
        "vmresume\n\t"
        "setna %[err]\n\t"
        "pop %%r15\n\t"
        "pop %%r14\n\t"
        "pop %%r13\n\t"
        "pop %%r12\n\t"
        "pop %%r11\n\t"
        "pop %%r10\n\t"
        "pop %%r9\n\t"
        "pop %%r8\n\t"
        "pop %%rbp\n\t"
        "pop %%rdi\n\t"
        "pop %%rsi\n\t"
        "pop %%rdx\n\t"
        "pop %%rcx\n\t"
        "pop %%rbx"
        : [err] "=rm"(error)
        :
        : "cc", "memory"
    );
    return !error;
}

// ═══════════════════════════════════════════════════════════════════════════
//  AMD SVM Operations
// ═══════════════════════════════════════════════════════════════════════════

bool VMM::SVMEnable() {
    if (virt_type != VIRT_AMD_SVM) return false;
    if (svm_enabled) return true;

    SerialLogger::Log("VMM: Enabling SVM...\r\n");

    // Set EFER.SVME (bit 12)
    uint64_t efer = ReadMSR(0xC0000080); // IA32_EFER
    efer |= (1 << 12);
    WriteMSR(0xC0000080, efer);

    // Allocate host save area (4KB aligned)
    void* hsave = AllocAligned(4096, 4096);
    if (!hsave) {
        SerialLogger::Log("VMM: Failed to allocate host save area\r\n");
        return false;
    }
    for (int i = 0; i < 4096; i++) ((uint8_t*)hsave)[i] = 0;

    // Set MSR_VM_HSAVE_PA
    WriteMSR(MSR_VM_HSAVE_PA, (uint64_t)(uintptr_t)hsave);

    svm_enabled = true;
    SerialLogger::Log("VMM: SVM enabled, host save area at ");
    SerialLogger::LogHex((uint32_t)(uintptr_t)hsave);
    SerialLogger::Log("\r\n");
    return true;
}

void VMM::SVMDisable() {
    if (!svm_enabled) return;
    uint64_t efer = ReadMSR(0xC0000080);
    efer &= ~(1ULL << 12);
    WriteMSR(0xC0000080, efer);
    svm_enabled = false;
    SerialLogger::Log("VMM: SVM disabled\r\n");
}

void VMM::VMRun(VMCB* vmcb) {
    if (!svm_enabled || !vmcb) return;
    uint64_t phys = (uint64_t)(uintptr_t)vmcb;
    // Save all host GPRs, load RAX with VMCB physical addr, VMRUN
    // After #VMEXIT the guest state is saved to VMCB automatically
    asm volatile(
        "push %%rbp\n\t"
        "push %%rbx\n\t"
        "push %%rcx\n\t"
        "push %%rdx\n\t"
        "push %%rsi\n\t"
        "push %%rdi\n\t"
        "push %%r8\n\t"
        "push %%r9\n\t"
        "push %%r10\n\t"
        "push %%r11\n\t"
        "push %%r12\n\t"
        "push %%r13\n\t"
        "push %%r14\n\t"
        "push %%r15\n\t"
        "mov %0, %%rax\n\t"
        "vmrun\n\t"
        "pop %%r15\n\t"
        "pop %%r14\n\t"
        "pop %%r13\n\t"
        "pop %%r12\n\t"
        "pop %%r11\n\t"
        "pop %%r10\n\t"
        "pop %%r9\n\t"
        "pop %%r8\n\t"
        "pop %%rdi\n\t"
        "pop %%rsi\n\t"
        "pop %%rdx\n\t"
        "pop %%rcx\n\t"
        "pop %%rbx\n\t"
        "pop %%rbp"
        :
        : "r"(phys)
        : "rax", "memory"
    );
}

// ═══════════════════════════════════════════════════════════════════════════
//  vCPU Management
// ═══════════════════════════════════════════════════════════════════════════

vCPU* VMM::CreateVCPU() {
    vCPU* cpu = (vCPU*)KernelHeap::Alloc(sizeof(vCPU));
    if (!cpu) return nullptr;

    for (unsigned i = 0; i < sizeof(vCPU); i++) ((uint8_t*)cpu)[i] = 0;
    cpu->vcpu_id = vcpu_count++;
    cpu->type = virt_type;
    cpu->launched = false;

    if (virt_type == VIRT_INTEL_VTX) {
        cpu->vmcs = (VMCSRegion*)AllocAligned(4096, 4096);
        if (!cpu->vmcs) { KernelHeap::Free(cpu); return nullptr; }
        for (int i = 0; i < 4096; i++) ((uint8_t*)cpu->vmcs)[i] = 0;
        cpu->vmcs->revision_id = vmx_revision_id;
        cpu->vmcb = nullptr;
    } else if (virt_type == VIRT_AMD_SVM) {
        cpu->vmcb = (VMCB*)AllocAligned(4096, 4096);
        if (!cpu->vmcb) { KernelHeap::Free(cpu); return nullptr; }
        for (int i = 0; i < 4096; i++) ((uint8_t*)cpu->vmcb)[i] = 0;
        cpu->vmcs = nullptr;
    }

    SerialLogger::Log("VMM: Created vCPU #");
    SerialLogger::LogDec(cpu->vcpu_id);
    SerialLogger::Log("\r\n");
    return cpu;
}

void VMM::DestroyVCPU(vCPU* cpu) {
    if (!cpu) return;
    if (cpu->vmcs) {
        if (vmx_on) VMClear(cpu->vmcs);
        KernelHeap::Free(cpu->vmcs);
    }
    if (cpu->vmcb) KernelHeap::Free(cpu->vmcb);
    vcpu_count--;
    KernelHeap::Free(cpu);
}

bool VMM::SetupVCPU(vCPU* cpu) {
    if (!cpu) return false;

    if (cpu->type == VIRT_INTEL_VTX) {
        if (!vmx_on && !VMXOn()) return false;
        if (!VMClear(cpu->vmcs)) return false;
        if (!VMPtrLoad(cpu->vmcs)) return false;
        SetupVMCSControls(cpu);
        SetupVMCSHostState(cpu);
        SetupVMCSGuestState(cpu);
        SerialLogger::Log("VMM: Intel VMCS configured for vCPU #");
        SerialLogger::LogDec(cpu->vcpu_id);
        SerialLogger::Log("\r\n");
    } else if (cpu->type == VIRT_AMD_SVM) {
        if (!svm_enabled && !SVMEnable()) return false;
        SetupVMCBControls(cpu);
        SetupVMCBGuestState(cpu);
        SerialLogger::Log("VMM: AMD VMCB configured for vCPU #");
        SerialLogger::LogDec(cpu->vcpu_id);
        SerialLogger::Log("\r\n");
    }
    return true;
}

int VMM::RunVCPU(vCPU* cpu) {
    if (!cpu) return -1;

    if (cpu->type == VIRT_INTEL_VTX) {
        // Load VMCS
        if (!VMPtrLoad(cpu->vmcs)) return -1;

        bool ok;
        if (!cpu->launched) {
            ok = VMLaunch();
            if (ok) cpu->launched = true;
        } else {
            ok = VMResume();
        }
        // On VM-exit we arrive here
        cpu->exit_reason = VMRead(VMCS_EXIT_REASON);
        cpu->exit_qualification = VMRead(VMCS_EXIT_QUALIFICATION);
        return (int)cpu->exit_reason;

    } else if (cpu->type == VIRT_AMD_SVM) {
        VMRun(cpu->vmcb);
        cpu->launched = true;
        cpu->exit_reason = (uint32_t)cpu->vmcb->exit_code;
        cpu->exit_qualification = (uint32_t)cpu->vmcb->exit_info1;
        return (int)cpu->exit_reason;
    }
    return -1;
}

// ═══════════════════════════════════════════════════════════════════════════
//  VMCS Setup (Intel VT-x)
// ═══════════════════════════════════════════════════════════════════════════

void VMM::SetupVMCSControls(vCPU* cpu) {
    (void)cpu;

    // Read allowed pin-based controls
    uint64_t pin_msr = ReadMSR(MSR_IA32_VMX_PINBASED_CTLS);
    uint32_t pin_allowed0 = (uint32_t)(pin_msr & 0xFFFFFFFF);
    uint32_t pin_allowed1 = (uint32_t)(pin_msr >> 32);
    uint32_t pin_controls = PIN_EXTERNAL_INT_EXIT | PIN_NMI_EXIT;
    pin_controls |= pin_allowed0;
    pin_controls &= pin_allowed1;
    VMWrite(VMCS_PIN_BASED_CONTROLS, pin_controls);

    // Primary processor-based controls
    uint64_t proc_msr = ReadMSR(MSR_IA32_VMX_PROCBASED_CTLS);
    uint32_t proc_allowed0 = (uint32_t)(proc_msr & 0xFFFFFFFF);
    uint32_t proc_allowed1 = (uint32_t)(proc_msr >> 32);
    uint32_t proc_controls = PROC_HLT_EXIT | PROC_IO_EXIT | PROC_SECONDARY_CONTROLS
                           | PROC_MOV_DR_EXIT | PROC_MSR_BITMAPS;
    proc_controls |= proc_allowed0;
    proc_controls &= proc_allowed1;
    VMWrite(VMCS_PROC_BASED_CONTROLS, proc_controls);

    // Secondary processor-based controls
    if (proc_controls & PROC_SECONDARY_CONTROLS) {
        uint64_t proc2_msr = ReadMSR(MSR_IA32_VMX_PROCBASED_CTLS2);
        uint32_t proc2_allowed0 = (uint32_t)(proc2_msr & 0xFFFFFFFF);
        uint32_t proc2_allowed1 = (uint32_t)(proc2_msr >> 32);
        uint32_t proc2_controls = PROC2_ENABLE_EPT | PROC2_ENABLE_RDTSCP
                                | PROC2_UNRESTRICTED_GUEST;
        proc2_controls |= proc2_allowed0;
        proc2_controls &= proc2_allowed1;
        VMWrite(VMCS_PROC_BASED_CONTROLS2, proc2_controls);
    }

    // VM-Exit controls
    uint64_t exit_msr = ReadMSR(MSR_IA32_VMX_EXIT_CTLS);
    uint32_t exit_allowed0 = (uint32_t)(exit_msr & 0xFFFFFFFF);
    uint32_t exit_allowed1 = (uint32_t)(exit_msr >> 32);
    uint32_t exit_controls = 0;
    exit_controls |= exit_allowed0;
    exit_controls &= exit_allowed1;
    VMWrite(VMCS_EXIT_CONTROLS, exit_controls);

    // VM-Entry controls
    uint64_t entry_msr = ReadMSR(MSR_IA32_VMX_ENTRY_CTLS);
    uint32_t entry_allowed0 = (uint32_t)(entry_msr & 0xFFFFFFFF);
    uint32_t entry_allowed1 = (uint32_t)(entry_msr >> 32);
    uint32_t entry_controls = 0;
    entry_controls |= entry_allowed0;
    entry_controls &= entry_allowed1;
    VMWrite(VMCS_ENTRY_CONTROLS, entry_controls);

    // Intercept only #DB (1) and #BP (3) exceptions — let guest handle others
    VMWrite(VMCS_EXCEPTION_BITMAP, (1 << 1) | (1 << 3));

    // VMCS Link Pointer — must be all-ones when not using VMCS shadowing
    VMWrite(VMCS_VMCS_LINK_PTR, 0xFFFFFFFF);
    // High 32 bits (VMCS link pointer is 64-bit natural-width field)
    VMWrite(VMCS_VMCS_LINK_PTR + 1, 0xFFFFFFFF);

    // Guest activity state = Active (0)
    VMWrite(VMCS_GUEST_ACTIVITY, 0);
    VMWrite(VMCS_GUEST_INTERRUPTIBILITY, 0);
}

void VMM::SetupVMCSHostState(vCPU* cpu) {
    (void)cpu;

    // Host CR0/CR3/CR4
    VMWrite(VMCS_HOST_CR0, ReadCR0());
    VMWrite(VMCS_HOST_CR3, ReadCR3());
    VMWrite(VMCS_HOST_CR4, ReadCR4());

    // Host selectors
    uint16_t cs, ss, ds, es, fs, gs, tr;
    asm volatile("mov %%cs, %0" : "=r"(cs));
    asm volatile("mov %%ss, %0" : "=r"(ss));
    asm volatile("mov %%ds, %0" : "=r"(ds));
    asm volatile("mov %%es, %0" : "=r"(es));
    asm volatile("mov %%fs, %0" : "=r"(fs));
    asm volatile("mov %%gs, %0" : "=r"(gs));
    asm volatile("str %0"       : "=r"(tr));
    VMWrite(VMCS_HOST_CS_SEL, cs);
    VMWrite(VMCS_HOST_SS_SEL, ss);
    VMWrite(VMCS_HOST_DS_SEL, ds);
    VMWrite(VMCS_HOST_ES_SEL, es);
    VMWrite(VMCS_HOST_FS_SEL, fs);
    VMWrite(VMCS_HOST_GS_SEL, gs);
    VMWrite(VMCS_HOST_TR_SEL, tr);

    // Host RIP — point to the VM-exit return stub
    // Host RSP — current stack pointer
    uint64_t rsp_val;
    asm volatile("mov %%rsp, %0" : "=r"(rsp_val));
    VMWrite(VMCS_HOST_RSP, rsp_val);
    VMWrite(VMCS_HOST_RIP, (uint64_t)(uintptr_t)&_vmm_host_return_stub);
}

void VMM::SetupVMCSGuestState(vCPU* cpu) {
    (void)cpu;

    // Basic guest state: real mode at 0x7C00 (like BIOS boot)
    VMWrite(VMCS_GUEST_CR0, 0x00000030); // PE=0, ET=1, NW=1
    VMWrite(VMCS_GUEST_CR3, 0);
    VMWrite(VMCS_GUEST_CR4, 0);

    // Guest selectors — real mode
    VMWrite(VMCS_GUEST_CS_SEL, 0x0000);
    VMWrite(VMCS_GUEST_CS_BASE, 0x00000000);
    VMWrite(VMCS_GUEST_CS_LIMIT, 0xFFFF);
    VMWrite(VMCS_GUEST_CS_AR, 0x009B);  // Present, DPL0, Code, R/X

    VMWrite(VMCS_GUEST_SS_SEL, 0x0000);
    VMWrite(VMCS_GUEST_SS_BASE, 0x00000000);
    VMWrite(VMCS_GUEST_SS_LIMIT, 0xFFFF);
    VMWrite(VMCS_GUEST_SS_AR, 0x0093);  // Present, DPL0, Data, R/W

    VMWrite(VMCS_GUEST_DS_SEL, 0x0000);
    VMWrite(VMCS_GUEST_DS_BASE, 0x00000000);
    VMWrite(VMCS_GUEST_DS_LIMIT, 0xFFFF);
    VMWrite(VMCS_GUEST_DS_AR, 0x0093);

    VMWrite(VMCS_GUEST_ES_SEL, 0x0000);
    VMWrite(VMCS_GUEST_ES_BASE, 0x00000000);
    VMWrite(VMCS_GUEST_ES_LIMIT, 0xFFFF);
    VMWrite(VMCS_GUEST_ES_AR, 0x0093);

    VMWrite(VMCS_GUEST_FS_SEL, 0x0000);
    VMWrite(VMCS_GUEST_FS_BASE, 0x00000000);
    VMWrite(VMCS_GUEST_FS_LIMIT, 0xFFFF);
    VMWrite(VMCS_GUEST_FS_AR, 0x0093);

    VMWrite(VMCS_GUEST_GS_SEL, 0x0000);
    VMWrite(VMCS_GUEST_GS_BASE, 0x00000000);
    VMWrite(VMCS_GUEST_GS_LIMIT, 0xFFFF);
    VMWrite(VMCS_GUEST_GS_AR, 0x0093);

    // TR (Task Register) — required to be present in VMCS
    VMWrite(VMCS_GUEST_TR_SEL, 0x0000);
    VMWrite(VMCS_GUEST_TR_BASE, 0x00000000);
    VMWrite(VMCS_GUEST_TR_LIMIT, 0xFFFF);
    VMWrite(VMCS_GUEST_TR_AR, 0x008B); // 32-bit busy TSS, present

    // LDTR
    VMWrite(VMCS_GUEST_LDTR_SEL, 0x0000);
    VMWrite(VMCS_GUEST_LDTR_BASE, 0x00000000);
    VMWrite(VMCS_GUEST_LDTR_LIMIT, 0xFFFF);
    VMWrite(VMCS_GUEST_LDTR_AR, 0x10000); // Unusable (bit 16)

    // GDTR/IDTR
    VMWrite(VMCS_GUEST_GDTR_BASE, 0);
    VMWrite(VMCS_GUEST_GDTR_LIMIT, 0xFFFF);
    VMWrite(VMCS_GUEST_IDTR_BASE, 0);
    VMWrite(VMCS_GUEST_IDTR_LIMIT, 0x3FF); // Real mode IVT

    // Guest RIP = 0x7C00 (boot sector), RSP = 0x7000, RFLAGS = 0x02
    VMWrite(VMCS_GUEST_RIP, 0x7C00);
    VMWrite(VMCS_GUEST_RSP, 0x7000);
    VMWrite(VMCS_GUEST_RFLAGS, 0x00000002);  // Bit 1 always set
}

// ═══════════════════════════════════════════════════════════════════════════
//  VMCB Setup (AMD-V / SVM)
// ═══════════════════════════════════════════════════════════════════════════

void VMM::SetupVMCBControls(vCPU* cpu) {
    if (!cpu || !cpu->vmcb) return;
    VMCB* vmcb = cpu->vmcb;

    // Intercept important operations
    vmcb->intercept_misc1 = SVM_INTERCEPT_INTR | SVM_INTERCEPT_NMI
                          | SVM_INTERCEPT_CPUID | SVM_INTERCEPT_HLT
                          | SVM_INTERCEPT_IOIO | SVM_INTERCEPT_MSR;

    // Intercept all CR0 writes
    vmcb->intercept_cr_write = (1 << 0); // CR0 write

    // Guest ASID (must be non-zero)
    vmcb->guest_asid = 1;

    // Enable nested paging
    vmcb->np_enable = 1;

    // TLB control: flush all
    vmcb->tlb_control = 1;
}

void VMM::SetupVMCBGuestState(vCPU* cpu) {
    if (!cpu || !cpu->vmcb) return;
    VMCB* vmcb = cpu->vmcb;

    // Real mode — start at 0x7C00
    vmcb->cs.selector = 0x0000;
    vmcb->cs.base     = 0x00000000;
    vmcb->cs.limit    = 0xFFFF;
    vmcb->cs.attrib   = 0x049B;  // Present, Read/Execute, DPL=0

    vmcb->ss.selector = 0x0000;
    vmcb->ss.base     = 0x00000000;
    vmcb->ss.limit    = 0xFFFF;
    vmcb->ss.attrib   = 0x0493;

    vmcb->ds = vmcb->ss;
    vmcb->es = vmcb->ss;
    vmcb->fs = vmcb->ss;
    vmcb->gs = vmcb->ss;

    // GDT and IDT — minimal real-mode
    vmcb->gdtr.base  = 0;
    vmcb->gdtr.limit = 0xFFFF;
    vmcb->idtr.base  = 0;
    vmcb->idtr.limit = 0x3FF;

    // Control registers
    vmcb->cr0    = 0x00000030;  // ET=1, NW=1 (no PE)
    vmcb->cr3    = 0;
    vmcb->cr4    = 0;
    vmcb->efer   = (1ULL << 12); // SVME must be set
    vmcb->dr6    = 0xFFFF0FF0;
    vmcb->dr7    = 0x00000400;

    // Execution state
    vmcb->rip    = 0x7C00;
    vmcb->rsp    = 0x7000;
    vmcb->rax    = 0;
    vmcb->rflags = 0x00000002;

    // PAT — default value
    vmcb->g_pat  = 0x0007040600070406ULL;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Debug / Status
// ═══════════════════════════════════════════════════════════════════════════

int VMM::GetActiveVCPUCount() { return vcpu_count; }

void VMM::DumpVCPUState(vCPU* cpu) {
    if (!cpu) return;
    SerialLogger::Log("=== vCPU #");
    SerialLogger::LogDec(cpu->vcpu_id);
    SerialLogger::Log(" State ===\r\n");

    SerialLogger::Log("  Type: ");
    SerialLogger::Log(cpu->type == VIRT_INTEL_VTX ? "Intel VT-x" : "AMD-V");
    SerialLogger::Log("\r\n");

    SerialLogger::Log("  Launched: ");
    SerialLogger::Log(cpu->launched ? "yes" : "no");
    SerialLogger::Log("\r\n");

    SerialLogger::Log("  Last exit reason: ");
    SerialLogger::LogHex(cpu->exit_reason);
    SerialLogger::Log("\r\n");

    SerialLogger::Log("  Exit qualification: ");
    SerialLogger::LogHex((uint32_t)cpu->exit_qualification);
    SerialLogger::Log("\r\n");

    if (cpu->type == VIRT_INTEL_VTX && vmx_on && cpu->vmcs) {
        VMPtrLoad(cpu->vmcs);
        SerialLogger::Log("  Guest RIP: ");
        SerialLogger::LogHex(VMRead(VMCS_GUEST_RIP));
        SerialLogger::Log("\r\n");
        SerialLogger::Log("  Guest RSP: ");
        SerialLogger::LogHex(VMRead(VMCS_GUEST_RSP));
        SerialLogger::Log("\r\n");
        SerialLogger::Log("  Guest CR0: ");
        SerialLogger::LogHex(VMRead(VMCS_GUEST_CR0));
        SerialLogger::Log("\r\n");
    } else if (cpu->type == VIRT_AMD_SVM && cpu->vmcb) {
        SerialLogger::Log("  Guest RIP: ");
        SerialLogger::LogHex((uint32_t)cpu->vmcb->rip);
        SerialLogger::Log("\r\n");
        SerialLogger::Log("  Guest RSP: ");
        SerialLogger::LogHex((uint32_t)cpu->vmcb->rsp);
        SerialLogger::Log("\r\n");
        SerialLogger::Log("  Guest CR0: ");
        SerialLogger::LogHex((uint32_t)cpu->vmcb->cr0);
        SerialLogger::Log("\r\n");
    }
}
