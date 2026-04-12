//  kurono os  -  virtual machine monitor implementation
//  intel vt-x / amd-v detection, vmcs/vmcb setup, vmx lifecycle
#include "vmm.h"
#include "../drivers/serial.h"
#include "../kernel/heap.h"

// global: pointer to current vcpu's register save area for vm-exit handler.
// set by runvcpu before vm entry so the inline vm-exit handler can save
// guest gprs without needing a register to hold the pointer.
// must be extern "c" so the inline asm 'movabs $g_vmx_guest_regs' finds
// the unmangled symbol name, and non-static so the linker can resolve it.
extern "C" uint64_t* volatile g_vmx_guest_regs = nullptr;

// same for amd svm  -  must be extern "c" for movabs in inline asm
extern "C" uint64_t* volatile g_svm_guest_regs = nullptr;

// vm-exit return stub  -  kept for amd-v and as a fallback.
// intel vt-x now uses an inline asm label for host_rip instead.
extern "C" void _vmm_host_return_stub() {
    // legacy stub  -  amd svm path still references this symbol.
}

// helper: allocate page-aligned memory from kernel heap
static void* AllocAligned(size_t size, size_t align) {
    // allocate extra for alignment + pointer storage
    void* raw = KernelHeap::Alloc(size + align + sizeof(void*));
    if (!raw) return nullptr;
    uintptr_t addr = (uintptr_t)raw + sizeof(void*);
    uintptr_t aligned = (addr + align - 1) & ~(align - 1);
    // store original pointer just before the aligned address for freeing
    ((void**)aligned)[-1] = raw;
    return (void*)aligned;
}

static void FreeAligned(void* aligned) {
    if (!aligned) return;
    void* raw = ((void**)aligned)[-1];
    KernelHeap::Free(raw);
}

VirtType VMM::virt_type       = VIRT_NONE;
bool     VMM::initialized     = false;
bool     VMM::vmx_on          = false;
bool     VMM::svm_enabled     = false;
bool     VMM::nested          = false;
bool     VMM::whpx_detected   = false;
bool     VMM::whpx_nested_ok  = false;
uint32_t VMM::max_hv_leaf     = 0;
int      VMM::vcpu_count      = 0;
uint32_t VMM::vmx_revision_id = 0;
uint32_t VMM::last_vm_instr_error = 0;
uint64_t VMM::last_vm_entry_guest_rip = 0;
uint64_t VMM::last_vm_entry_guest_cr0 = 0;
uint64_t VMM::last_vm_entry_guest_cr4 = 0;
char     VMM::vendor_string[16] = {0};
char     VMM::hv_vendor_string[16] = {0};

//  low-level cpu intrinsics  -  inline asm for x86_64

static inline void cpuid(uint32_t leaf, uint32_t& eax, uint32_t& ebx,
                          uint32_t& ecx, uint32_t& edx) {
    asm volatile("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(leaf), "c"(0));
}

struct PackedDescriptorTablePtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static uint64_t ReadFSBase() {
    return VMM::ReadMSR(0xC0000100);
}

static uint64_t ReadGSBase() {
    return VMM::ReadMSR(0xC0000101);
}

static uint64_t ReadTRBase(uint16_t tr_selector) {
    if ((tr_selector & 0xFFF8) == 0) return 0;

    PackedDescriptorTablePtr gdtr;
    asm volatile("sgdt %0" : "=m"(gdtr));

    uint16_t index = (uint16_t)(tr_selector & 0xFFF8);
    if ((uint32_t)index + 15 > gdtr.limit) return 0;

    uint8_t* desc = (uint8_t*)(uintptr_t)(gdtr.base + index);
    uint64_t base = 0;
    base |= ((uint64_t)desc[2]);
    base |= ((uint64_t)desc[3]) << 8;
    base |= ((uint64_t)desc[4]) << 16;
    base |= ((uint64_t)desc[7]) << 24;
    base |= ((uint64_t)desc[8]) << 32;
    base |= ((uint64_t)desc[9]) << 40;
    base |= ((uint64_t)desc[10]) << 48;
    base |= ((uint64_t)desc[11]) << 56;
    return base;
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

static inline void write_cr0(uint64_t val) {
    asm volatile("mov %0, %%cr0" : : "r"(val));
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

//  initialization & detection

// safe msr read  -  returns false if the msr cannot be read (e.g. #gp).
// under whpx, some amd msrs are not emulated on intel hosts and vice
// versa, so a plain rdmsr would triple-fault the guest.  we install a
// tiny #gp handler via the idt that sets a flag and skips the faulting
// instruction.  this is only used during init/detection.
//
// implementation: we *cannot* easily install a temporary idt handler in
// a bare-metal kernel that may not have its idt fully set up yet.
// instead, when running under hyper-v (whpx), we use the hypervisor's
// cpuid leaves to determine capabilities without touching unsafe msrs.
// when not under a hypervisor, the msrs are real and safe to read.

bool VMM::SafeReadMSR(uint32_t msr, uint64_t& out) {
    // under whpx, only read msrs we know are emulated
    if (whpx_detected) {
        // hyper-v emulates standard intel msrs
        // amd msrs (0xc000xxxx) are not emulated on intel whpx hosts
        // and will cause #gp → guest crash.
        if (msr >= 0xC0000000 && msr != 0xC0000080) {
            // 0xc0000080 = ia32_efer  -  universally emulated
            // all other 0xcxxxxxxx msrs (vm_cr, vm_hsave_pa, etc.) are
            // amd-specific and unsafe under whpx on intel hosts.
            SerialLogger::Log("VMM: Skipping unsafe AMD MSR 0x");
            SerialLogger::LogHex(msr);
            SerialLogger::Log(" under WHPX\r\n");
            out = 0;
            return false;
        }
    }
    out = ReadMSR(msr);
    return true;
}

void VMM::DetectHypervisor() {
    uint32_t eax, ebx, ecx, edx;

    // cpuid leaf 0x40000000  -  hypervisor vendor string
    cpuid(0x40000000, eax, ebx, ecx, edx);

    // eax = max hypervisor leaf supported
    max_hv_leaf = eax;
    // ebx:ecx:edx = vendor string (12 chars)
    *(uint32_t*)&hv_vendor_string[0] = ebx;
    *(uint32_t*)&hv_vendor_string[4] = ecx;
    *(uint32_t*)&hv_vendor_string[8] = edx;
    hv_vendor_string[12] = 0;

    bool hv_leaf_valid = (max_hv_leaf >= 0x40000001 && max_hv_leaf <= 0x400000FF);
    bool hv_vendor_present = (ebx | ecx | edx) != 0;

    if (!hv_leaf_valid || !hv_vendor_present) {
        nested = false;
        whpx_detected = false;
        whpx_nested_ok = false;
        max_hv_leaf = 0;
        hv_vendor_string[0] = 0;
        SerialLogger::Log("VMM: Hypervisor CPUID hint was not backed by valid hypervisor leaves; treating system as bare metal\r\n");
        return;
    }

    nested = true;

    SerialLogger::Log("VMM: Hypervisor vendor: '");
    SerialLogger::Log(hv_vendor_string);
    SerialLogger::Log("' (max leaf 0x");
    SerialLogger::LogHex(max_hv_leaf);
    SerialLogger::Log(")\r\n");

    // hex dump vendor bytes for debugging (sometimes they're non-printable)
    SerialLogger::Log("VMM: HV CPUID 0x40000000: EBX=0x");
    SerialLogger::LogHex(ebx);
    SerialLogger::Log(" ECX=0x");
    SerialLogger::LogHex(ecx);
    SerialLogger::Log(" EDX=0x");
    SerialLogger::LogHex(edx);
    SerialLogger::Log("\r\n");

    // detection methods (in priority order):
    //   1. vendor string starts with "micr" (standard hyper-v)
    //   2. if vendor string is blank/garbled but max_hv_leaf >= 0x40000006
    //      → only hyper-v reports that many leaves; qemu under whpx may
    //        not pass through the vendor string correctly.
    //   3. kvm max leaf is typically 0x40000001, tcg has no hv leaves.

    bool vendor_is_msft = (hv_vendor_string[0] == 'M' && hv_vendor_string[1] == 'i' &&
                           hv_vendor_string[2] == 'c' && hv_vendor_string[3] == 'r');
    bool high_hv_leaves = (max_hv_leaf >= 0x40000006);

    if (vendor_is_msft || high_hv_leaves) {
        whpx_detected = true;
        whpx_nested_ok = false;
        if (vendor_is_msft) {
            SerialLogger::Log("VMM: Microsoft Hyper-V / WHPX detected (vendor string)\r\n");
        } else {
            SerialLogger::Log("VMM: Hyper-V / WHPX detected (max leaf 0x");
            SerialLogger::LogHex(max_hv_leaf);
            SerialLogger::Log(" implies Hyper-V)\r\n");
        }

        //
        // hyper-v cpuid leaf 0x4000000a = hv_cpuid_nested_features
        // if the hypervisor reports max leaf >= 0x4000000a, nested virt
        // may be available.  we read that leaf and check for features.
        //
        // important: even with max_hv_leaf >= 0x4000000a, qemu's whpx
        // accelerator does not implement nested svm/vmx.  the leaf might
        // exist but contain zeros.  only real hyper-v vms with
        // -exposevirtualizationextensions $true support actual nested virt.
        //
        // if the leaf is absent (max < 0x4000000a) or eax=0, nested virt
        // is not supported and vmrun/vmlaunch will crash the vm.

        if (max_hv_leaf >= 0x4000000A) {
            cpuid(0x4000000A, eax, ebx, ecx, edx);
            SerialLogger::Log("VMM: HV nested features (0x4000000A): EAX=0x");
            SerialLogger::LogHex(eax);
            SerialLogger::Log(" EBX=0x");
            SerialLogger::LogHex(ebx);
            SerialLogger::Log("\r\n");

            // hyper-v nested features leaf:
            //   eax bit 0 = direct virtual flush (enlightened tlb)
            //   eax bit 2 = enlightened msr bitmap
            //   eax bit 3 = page fault handling
            //   eax bit 18 = enlightened vmcs version (if non-zero → nested vmx)
            //   the mere presence of non-zero bits means the l0 hypervisor
            //   actively supports nested virt.
            if (eax != 0) {
                whpx_nested_ok = true;
                SerialLogger::Log("VMM: WHPX NESTED VIRT SUPPORTED  -  vmrun/vmlaunch safe\r\n");
            } else {
                SerialLogger::Log("VMM: Nested leaf present but EAX=0  -  nested NOT supported\r\n");
                SerialLogger::Log("VMM: vmrun/vmlaunch would hang or crash the VM\r\n");
            }
        } else {
            SerialLogger::Log("VMM: Max HV leaf 0x");
            SerialLogger::LogHex(max_hv_leaf);
            SerialLogger::Log(" < 0x4000000A  -  NO nested virt\r\n");
            SerialLogger::Log("VMM: vmrun/vmlaunch would crash the VM (VP exit code 4)\r\n");
        }

        // also log partition features for diagnostics
        if (max_hv_leaf >= 0x40000003) {
            cpuid(0x40000003, eax, ebx, ecx, edx);
            SerialLogger::Log("VMM: HV partition features (0x40000003): EAX=0x");
            SerialLogger::LogHex(eax);
            SerialLogger::Log(" EDX=0x");
            SerialLogger::LogHex(edx);
            SerialLogger::Log("\r\n");
        }
    }
    // check for kvmkvmkvm (kvm), tcgtcgtcgtcg (tcg/qemu emulated)
    else if (hv_vendor_string[0] == 'K' && hv_vendor_string[1] == 'V' &&
             hv_vendor_string[2] == 'M') {
        SerialLogger::Log("VMM: KVM hypervisor detected\r\n");
    }
    else if (hv_vendor_string[0] == 'T' && hv_vendor_string[1] == 'C' &&
             hv_vendor_string[2] == 'G') {
        SerialLogger::Log("VMM: QEMU/TCG (software) detected\r\n");
    }
}

bool VMM::DetectWHPXNestedVirt() {
    if (!whpx_detected) return false;

    // under whpx, check what nested virt the host actually supports.
    // hyper-v leaf 0x40000003 edx bit 4 = nested virt available.
    uint32_t eax, ebx, ecx, edx;
    cpuid(0x40000003, eax, ebx, ecx, edx);

    bool nested_supported = (edx & (1 << 4)) != 0;

    // even if the hyper-v feature flag is not set, qemu+whpx may still
    // expose vt-x/svm via cpuid.  the real test is whether the enable
    // operation (vmxon / efer.svme) succeeds at runtime.  we return
    // true here if cpuid says either vt-x or svm is available; the
    // actual enable will be attempted later with fallback.
    if (!nested_supported) {
        // fall back to checking cpuid directly
        if (DetectVTx() || DetectSVM()) {
            SerialLogger::Log("VMM: WHPX nested flag absent but CPUID shows virt bits\r\n");
            return true;
        }
    }
    return nested_supported;
}

void VMM::Init() {
    if (initialized) return;
    initialized = true;
    vcpu_count = 0;
    vmx_on = false;
    svm_enabled = false;
    nested = false;
    whpx_detected = false;
    whpx_nested_ok = false;
    max_hv_leaf = 0;
    last_vm_instr_error = 0;
    last_vm_entry_guest_rip = 0;
    last_vm_entry_guest_cr0 = 0;
    last_vm_entry_guest_cr4 = 0;
    hv_vendor_string[0] = 0;

    SerialLogger::Log("VMM: Initializing...\r\n");

    // get cpu vendor string
    uint32_t eax, ebx, ecx, edx;
    cpuid(0, eax, ebx, ecx, edx);
    *(uint32_t*)&vendor_string[0] = ebx;
    *(uint32_t*)&vendor_string[4] = edx;
    *(uint32_t*)&vendor_string[8] = ecx;
    vendor_string[12] = 0;

    SerialLogger::Log("VMM: CPU Vendor: ");
    SerialLogger::Log(vendor_string);
    SerialLogger::Log("\r\n");

    // check for hypervisor presence (cpuid.1.ecx bit 31)
    cpuid(1, eax, ebx, ecx, edx);
    if (ecx & CPUID_HYPERVISOR_BIT) {
        SerialLogger::Log("VMM: Hypervisor CPUID bit set  -  probing hypervisor leaves\r\n");
        DetectHypervisor();
        if (nested) {
            SerialLogger::Log("VMM: Running under a hypervisor (nested virt)\r\n");
        }
    }

    // strategy:
    //   1. check vt-x cpuid bit (works on intel real hw and hyper-v intel hosts)
    //   2. check svm cpuid bit (works on amd real hw and qemu +svm)
    //   3. under whpx: be careful with msr reads  -  intel msrs only on intel,
    //      amd msrs only on amd.  whpx on intel host + qemu -cpu qemu64,+svm
    //      will expose svm cpuid bit but amd msrs are not emulated.
    //
    // we try vt-x first (preferred under whpx on intel hosts), then svm.

    bool vtx_cpuid = DetectVTx();
    bool svm_cpuid = DetectSVM();

    SerialLogger::Log("VMM: CPUID  -  VT-x: ");
    SerialLogger::Log(vtx_cpuid ? "YES" : "no");
    SerialLogger::Log(", SVM: ");
    SerialLogger::Log(svm_cpuid ? "YES" : "no");
    SerialLogger::Log("\r\n");

    if (vtx_cpuid) {
        // intel vt-x path
        SerialLogger::Log("VMM: Intel VT-x detected via CPUID\r\n");

        uint64_t feature_control = 0;
        bool fc_ok = SafeReadMSR(MSR_IA32_FEATURE_CONTROL, feature_control);
        bool vtx_usable = false;

        if (fc_ok) {
            SerialLogger::Log("VMM: IA32_FEATURE_CONTROL = 0x");
            SerialLogger::LogHex((uint32_t)feature_control);
            SerialLogger::Log("\r\n");

            if (!(feature_control & FEATURE_CONTROL_LOCKED)) {
                // unlocked  -  we can enable vt-x ourselves (bare metal)
                SerialLogger::Log("VMM: Feature control unlocked  -  will lock with VMXON enabled\r\n");
                vtx_usable = true;
            } else if (feature_control & FEATURE_CONTROL_VMXON) {
                // locked with vmxon enabled  -  bios enabled vt-x for us
                SerialLogger::Log("VMM: VT-x enabled in BIOS/firmware\r\n");
                vtx_usable = true;
            } else {
                // locked with vmxon disabled  -  bios intentionally blocked vt-x
                SerialLogger::Log("VMM: VT-x LOCKED OUT by firmware (VMXON bit clear)\r\n");
                if (whpx_detected) {
                    // under whpx, l0 manages vmx  -  vmxon bit in guest msr is irrelevant
                    SerialLogger::Log("VMM: WHPX manages VMX  -  proceeding anyway\r\n");
                    vtx_usable = true;
                } else {
                    SerialLogger::Log("VMM: Enable Intel Virtualization Technology in BIOS\r\n");
                }
            }
        } else {
            // can't read the msr  -  assume vt-x is available (best-effort)
            SerialLogger::Log("VMM: Cannot read IA32_FEATURE_CONTROL  -  assuming VT-x available\r\n");
            vtx_usable = true;
        }

        if (vtx_usable) {
            virt_type = VIRT_INTEL_VTX;
            uint64_t vmx_basic = 0;
            if (SafeReadMSR(MSR_IA32_VMX_BASIC, vmx_basic)) {
                vmx_revision_id = (uint32_t)(vmx_basic & 0x7FFFFFFF);
                SerialLogger::Log("VMM: VMX Revision ID: 0x");
                SerialLogger::LogHex(vmx_revision_id);
                SerialLogger::Log("\r\n");
            }
            SerialLogger::Log("VMM: Intel VT-x ready\r\n");
        } else {
            virt_type = VIRT_NONE;
            SerialLogger::Log("VMM: VT-x detected but not usable\r\n");
        }
    } else if (svm_cpuid) {
        // amd svm path
        SerialLogger::Log("VMM: AMD-V (SVM) bit present in CPUID\r\n");

        // under whpx, qemu -cpu qemu64,+svm exposes svm in cpuid but:
        //   1. amd msrs (vm_cr, vm_hsave_pa) may not be emulated → #gp
        //   2. even if efer.svme writes succeed, `vmrun` causes vp exit
        //      code 4 unless the host hyper-v actually supports nested virt
        //      (leaf 0x4000000a). qemu's whpx does not implement nested virt.
        //
        // we detect svm as present (for reporting), but only mark it
        // usable if nested virt is confirmed via hyper-v cpuid.
        if (whpx_detected) {
            SerialLogger::Log("VMM: WHPX + SVM  -  skipping VM_CR MSR check\r\n");
            if (whpx_nested_ok) {
                SerialLogger::Log("VMM: WHPX nested virt CONFIRMED  -  SVM usable\r\n");
                virt_type = VIRT_AMD_SVM;
            } else {
                SerialLogger::Log("VMM: WHPX nested virt NOT supported by host\r\n");
                SerialLogger::Log("VMM: SVM detected in CPUID but vmrun would crash\r\n");
                SerialLogger::Log("VMM: Marking HW virt as unavailable to avoid VP exit code 4\r\n");
                virt_type = VIRT_NONE;
            }
        } else {
            // bare metal or kvm  -  safe to read amd msrs
            uint64_t vm_cr = 0;
            if (SafeReadMSR(MSR_VM_CR, vm_cr)) {
                if ((vm_cr & (1 << 4)) == 0) {
                    SerialLogger::Log("VMM: SVM enabled (VM_CR.SVMDIS=0)\r\n");
                    virt_type = VIRT_AMD_SVM;
                } else {
                    SerialLogger::Log("VMM: SVM disabled in BIOS (VM_CR.SVMDIS=1)\r\n");
                }
            } else {
                SerialLogger::Log("VMM: Cannot read VM_CR  -  assuming SVM available\r\n");
                virt_type = VIRT_AMD_SVM;
            }
        }

        if (virt_type == VIRT_AMD_SVM) {
            SerialLogger::Log("VMM: AMD-V (SVM) activated\r\n");
        }
    } else {
        virt_type = VIRT_NONE;
        SerialLogger::Log("VMM: No hardware virtualization support detected\r\n");

        // under whpx, try one more thing: hyper-v may not set the standard
        // cpuid bits but still support nested virt via enlightenments.
        if (whpx_detected && DetectWHPXNestedVirt()) {
            SerialLogger::Log("VMM: WHPX nested virt available via enlightenments\r\n");
            // hyper-v on intel → use vt-x as the interface
            virt_type = VIRT_INTEL_VTX;
        }
    }

    SerialLogger::Log("VMM: Initialization complete  -  type=");
    SerialLogger::Log(virt_type == VIRT_INTEL_VTX ? "VT-x" :
                      virt_type == VIRT_AMD_SVM   ? "SVM"  : "NONE");
    if (whpx_detected) SerialLogger::Log(" [WHPX]");
    if (nested) SerialLogger::Log(" [nested]");
    SerialLogger::Log("\r\n");
}

bool VMM::IsSupported() { return virt_type != VIRT_NONE; }
bool VMM::IsNested() { return nested; }
uint32_t VMM::GetLastVMInstructionError() { return last_vm_instr_error; }
uint64_t VMM::GetLastVMEntryGuestRip() { return last_vm_entry_guest_rip; }
uint64_t VMM::GetLastVMEntryGuestCr0() { return last_vm_entry_guest_cr0; }
uint64_t VMM::GetLastVMEntryGuestCr4() { return last_vm_entry_guest_cr4; }
bool VMM::IsWHPX() { return whpx_detected; }
bool VMM::IsWHPXNestedOk() { return whpx_nested_ok; }
const char* VMM::GetHypervisorVendor() { return hv_vendor_string; }
VirtType VMM::GetType() { return virt_type; }
const char* VMM::GetVendor() { return vendor_string; }

//  feature detection

bool VMM::DetectVTx() {
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, eax, ebx, ecx, edx);
    return (ecx & CPUID_VMX_BIT) != 0;
}

bool VMM::DetectSVM() {
    uint32_t eax, ebx, ecx, edx;
    // check amd extended features
    cpuid(0x80000001, eax, ebx, ecx, edx);
    return (ecx & CPUID_SVM_BIT) != 0;
}

bool VMM::IsVTxEnabled() {
    if (!DetectVTx()) return false;
    uint64_t feature_control = 0;
    if (!SafeReadMSR(MSR_IA32_FEATURE_CONTROL, feature_control)) {
        // cannot read the msr; under whpx this is expected  -  assume available
        return whpx_detected;
    }
    // must be locked and vmxon enabled outside smx
    if (!(feature_control & FEATURE_CONTROL_LOCKED)) {
        // not locked  -  we could enable it by writing the msr
        return true;  // potentially available
    }
    return (feature_control & FEATURE_CONTROL_VMXON) != 0;
}

bool VMM::IsSVMEnabled() {
    if (!DetectSVM()) return false;
    // under whpx, amd msrs may not be emulated  -  skip the check
    if (whpx_detected) return true;
    uint64_t vm_cr = 0;
    if (!SafeReadMSR(MSR_VM_CR, vm_cr)) return true; // assume available
    // bit 4 = svmdis (svm disabled when set)
    return (vm_cr & (1 << 4)) == 0;
}

uint32_t VMM::GetVMXRevisionId() { return vmx_revision_id; }

//  intel vmx operations

bool VMM::VMXOn() {
    if (virt_type != VIRT_INTEL_VTX) return false;
    if (vmx_on) return true;

    SerialLogger::Log("VMM: Entering VMX root mode...");
    if (nested) SerialLogger::Log(" (nested)");
    SerialLogger::Log("\r\n");

    uint64_t old_cr0 = ReadCR0();
    uint64_t old_cr4 = ReadCR4();

    // ensure cr0/cr4 satisfy intel's vmx fixed-bit requirements.
    // if these are wrong, vmxon raises #gp on real hardware.
    uint64_t cr0_fixed0 = 0, cr0_fixed1 = ~0ull;
    uint64_t cr4_fixed0 = 0, cr4_fixed1 = ~0ull;
    if (!SafeReadMSR(MSR_IA32_VMX_CR0_FIXED0, cr0_fixed0) ||
        !SafeReadMSR(MSR_IA32_VMX_CR0_FIXED1, cr0_fixed1) ||
        !SafeReadMSR(MSR_IA32_VMX_CR4_FIXED0, cr4_fixed0) ||
        !SafeReadMSR(MSR_IA32_VMX_CR4_FIXED1, cr4_fixed1)) {
        SerialLogger::Log("VMM: Cannot read VMX fixed-bit MSRs\r\n");
        return false;
    }

    uint64_t cr0 = old_cr0;
    uint64_t cr4 = old_cr4;
    cr0 |= cr0_fixed0;
    cr0 &= cr0_fixed1;
    cr4 |= cr4_fixed0;
    cr4 &= cr4_fixed1;
    cr4 |= CR4_VMXE;
    write_cr0(cr0);
    WriteCR4(cr4);

    // ensure feature control msr allows vmxon.
    uint64_t fc = 0;
    if (!SafeReadMSR(MSR_IA32_FEATURE_CONTROL, fc)) {
        SerialLogger::Log("VMM: Cannot read IA32_FEATURE_CONTROL\r\n");
        WriteCR4(old_cr4);
        write_cr0(old_cr0);
        return false;
    }
    if (!(fc & FEATURE_CONTROL_LOCKED)) {
        fc |= FEATURE_CONTROL_LOCKED | FEATURE_CONTROL_VMXON;
        WriteMSR(MSR_IA32_FEATURE_CONTROL, fc);
    } else if ((fc & FEATURE_CONTROL_VMXON) == 0) {
        SerialLogger::Log("VMM: VMXON blocked by firmware\r\n");
        WriteCR4(old_cr4);
        write_cr0(old_cr0);
        return false;
    }

    // allocate vmxon region (4kb aligned)
    VMCSRegion* vmxon_region = (VMCSRegion*)AllocAligned(4096, 4096);
    if (!vmxon_region) {
        SerialLogger::Log("VMM: Failed to allocate VMXON region\r\n");
        WriteCR4(old_cr4);
        write_cr0(old_cr0);
        return false;
    }

    // clear and set revision id
    for (int i = 0; i < 4096; i++) ((uint8_t*)vmxon_region)[i] = 0;
    vmxon_region->revision_id = vmx_revision_id;

    // vmxon instruction
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
        FreeAligned(vmxon_region);
        WriteCR4(old_cr4);
        write_cr0(old_cr0);
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

    // clear vmxe in cr4
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
    if (virt_type != VIRT_INTEL_VTX || !vmx_on) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            SerialLogger::Log("VMM: VMWrite skipped  -  VMX not active\r\n");
        }
        (void)field;
        (void)value;
        return;
    }
    uint64_t f64 = field;  // vmwrite requires natural-width operands in 64-bit mode
    asm volatile("vmwrite %1, %0" : : "r"(f64), "rm"(value) : "cc");
}

uint64_t VMM::VMRead(uint32_t field) {
    if (virt_type != VIRT_INTEL_VTX || !vmx_on) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            SerialLogger::Log("VMM: VMRead skipped  -  VMX not active\r\n");
        }
        (void)field;
        return 0;
    }
    uint64_t value = 0;
    uint64_t f64 = field;  // vmread requires natural-width operands in 64-bit mode
    asm volatile("vmread %1, %0" : "=rm"(value) : "r"(f64) : "cc");
    return value;
}

bool VMM::VMLaunch() {
    if (virt_type != VIRT_INTEL_VTX || !vmx_on) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            SerialLogger::Log("VMM: VMLaunch skipped  -  VMX not active\r\n");
        }
        return false;
    }
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
    // if we get here, vmlaunch failed (success = control transferred to guest)
    return !error;
}

bool VMM::VMResume() {
    if (virt_type != VIRT_INTEL_VTX || !vmx_on) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            SerialLogger::Log("VMM: VMResume skipped  -  VMX not active\r\n");
        }
        return false;
    }
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

//  amd svm operations

bool VMM::SVMEnable() {
    if (virt_type != VIRT_AMD_SVM) return false;
    if (svm_enabled) return true;

    SerialLogger::Log("VMM: Enabling SVM...");
    if (nested) SerialLogger::Log(" [NESTED]");
    if (whpx_detected) SerialLogger::Log(" [WHPX]");
    SerialLogger::Log("\r\n");

    // set efer.svme (bit 12)
    uint64_t efer = ReadMSR(0xC0000080); // ia32_efer
    efer |= (1 << 12);
    WriteMSR(0xC0000080, efer);

    // verify the write actually took effect  - 
    // some hypervisors silently ignore the svme bit
    uint64_t efer_check = ReadMSR(0xC0000080);
    if (!(efer_check & (1 << 12))) {
        SerialLogger::Log("VMM: EFER.SVME write rejected  -  SVM not truly available\r\n");

        // under whpx, this means the host doesn't support nested svm
        // (likely an intel host using qemu -cpu qemu64,+svm).
        // the cpuid bit was set by qemu but the hypervisor blocks svme.
        if (whpx_detected) {
            SerialLogger::Log("VMM: WHPX on Intel host  -  SVM emulated in CPUID only\r\n");
            SerialLogger::Log("VMM: Falling back: will mark HW as unavailable\r\n");
            virt_type = VIRT_NONE;
        }
        return false;
    }

    // allocate host save area (4kb aligned)
    void* hsave = AllocAligned(4096, 4096);
    if (!hsave) {
        SerialLogger::Log("VMM: Failed to allocate host save area\r\n");
        return false;
    }
    for (int i = 0; i < 4096; i++) ((uint8_t*)hsave)[i] = 0;

    // set msr_vm_hsave_pa  -  under whpx on intel this msr doesn't exist
    if (whpx_detected) {
        // try to write it; if it fails, svm won't work
        // (we can't easily catch #gp, so we just write and hope for the best.
        //  on a real amd host under whpx, this will succeed.)
        SerialLogger::Log("VMM: Writing VM_HSAVE_PA under WHPX  -  may fault on Intel\r\n");
    }
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
    // save all host gprs, load rax with vmcb physical addr, vmrun
    // after #vmexit the guest state is saved to vmcb automatically
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

//  vcpu management

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
        FreeAligned(cpu->vmcs);
    }
    if (cpu->vmcb) FreeAligned(cpu->vmcb);
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

    last_vm_instr_error = 0;
    last_vm_entry_guest_rip = 0;
    last_vm_entry_guest_cr0 = 0;
    last_vm_entry_guest_cr4 = 0;

    if (cpu->type == VIRT_INTEL_VTX) {
        // load vmcs
        if (!VMPtrLoad(cpu->vmcs)) return -1;

        uint64_t launch_val = cpu->launched ? 0 : 1;  // 1 = vmlaunch, 0 = vmresume
        uint64_t* regs = cpu->regs;
        uint64_t result;

        // set up the global pointer so the vm-exit handler (label 3 in the
        // inline asm below) can save guest gprs back to cpu->regs[].
        g_vmx_guest_regs = regs;

        // register layout in regs[]  -  intel standard (modrm) ordering:
        //   0=rax, 1=rcx, 2=rdx, 3=rbx, 4=(rsp - in vmcs), 5=rbp,
        //   6=rsi, 7=rdi, 8=r8, 9=r9, ..., 15=r15
        // rsp and rip live in vmcs guest-state fields, not in regs[].

        asm volatile(
            "push %%rbx\n\t"
            "push %%rbp\n\t"
            "push %%r12\n\t"
            "push %%r13\n\t"
            "push %%r14\n\t"
            "push %%r15\n\t"

            // on vm-exit the cpu restores rsp from this value, so we
            // land back on our stack with callee-saved regs below us.
            "mov %%rsp, %%rax\n\t"
            "mov $0x6C14, %%rdx\n\t"        // vmcs_host_rsp
            "vmwrite %%rax, %%rdx\n\t"

            // on vm-exit the cpu jumps here so we can save guest gprs.
            "lea 3f(%%rip), %%rax\n\t"
            "mov $0x6C16, %%rdx\n\t"        // vmcs_host_rip
            "vmwrite %%rax, %%rdx\n\t"

            // after loading, callee-saved regs hold guest values, which
            // would invalidate rbp-relative memory operands and the
            // register holding %[regs].
            "test %[launch], %[launch]\n\t"

            // %[regs] is in a callee-saved register chosen by gcc.
            // we must copy it to rax before loading guest gprs, because
            // the callee-saved reg will be overwritten with the guest value.
            "mov %[regs], %%rax\n\t"

            "mov 8(%%rax),  %%rcx\n\t"      // regs[1]  = rcx
            "mov 16(%%rax), %%rdx\n\t"      // regs[2]  = rdx
            "mov 24(%%rax), %%rbx\n\t"      // regs[3]  = rbx
            // regs[4] = rsp  -  loaded by vmcs, skip
            "mov 40(%%rax), %%rbp\n\t"      // regs[5]  = rbp
            "mov 48(%%rax), %%rsi\n\t"      // regs[6]  = rsi
            "mov 56(%%rax), %%rdi\n\t"      // regs[7]  = rdi
            "mov 64(%%rax), %%r8\n\t"       // regs[8]  = r8
            "mov 72(%%rax), %%r9\n\t"       // regs[9]  = r9
            "mov 80(%%rax), %%r10\n\t"      // regs[10] = r10
            "mov 88(%%rax), %%r11\n\t"      // regs[11] = r11
            "mov 96(%%rax), %%r12\n\t"      // regs[12] = r12
            "mov 104(%%rax),%%r13\n\t"      // regs[13] = r13
            "mov 112(%%rax),%%r14\n\t"      // regs[14] = r14
            "mov 120(%%rax),%%r15\n\t"      // regs[15] = r15
            "mov 0(%%rax),  %%rax\n\t"      // regs[0]  = rax (must be last)

            "jz 1f\n\t"                     // zf=1 → launch==0 → vmresume
            "vmlaunch\n\t"
            "jmp 2f\n\t"                    // vmlaunch failed
            "1:\n\t"
            "vmresume\n\t"
            // fall through = vmresume failed

            // cf or zf is set, we never entered the guest.
            "2:\n\t"
            "mov $1, %%rax\n\t"             // rax = 1 (error flag)
            "jmp 4f\n\t"

            // cpu jumped here after a vm-exit.  all gprs hold guest
            // values.  rsp = host_rsp (our stack after callee pushes).
            "3:\n\t"
            // save guest rax to stack; we need a scratch register.
            "push %%rax\n\t"

            // load pointer to regs[] from the global.
            // mcmodel=large → use movabs for a 64-bit absolute address.
            "movabs $g_vmx_guest_regs, %%rax\n\t"
            "mov (%%rax), %%rax\n\t"

            // save guest gprs  -  intel standard (modrm) order
            "mov %%rcx, 8(%%rax)\n\t"       // regs[1]  = rcx
            "mov %%rdx, 16(%%rax)\n\t"      // regs[2]  = rdx
            "mov %%rbx, 24(%%rax)\n\t"      // regs[3]  = rbx
            // regs[4] = rsp  -  in vmcs, skip
            "mov %%rbp, 40(%%rax)\n\t"      // regs[5]  = rbp
            "mov %%rsi, 48(%%rax)\n\t"      // regs[6]  = rsi
            "mov %%rdi, 56(%%rax)\n\t"      // regs[7]  = rdi
            "mov %%r8,  64(%%rax)\n\t"      // regs[8]  = r8
            "mov %%r9,  72(%%rax)\n\t"      // regs[9]  = r9
            "mov %%r10, 80(%%rax)\n\t"      // regs[10] = r10
            "mov %%r11, 88(%%rax)\n\t"      // regs[11] = r11
            "mov %%r12, 96(%%rax)\n\t"      // regs[12] = r12
            "mov %%r13, 104(%%rax)\n\t"     // regs[13] = r13
            "mov %%r14, 112(%%rax)\n\t"     // regs[14] = r14
            "mov %%r15, 120(%%rax)\n\t"     // regs[15] = r15

            // recover guest rax from the stack and save it
            "pop %%rcx\n\t"                 // guest rax
            "mov %%rcx, 0(%%rax)\n\t"       // regs[0]  = rax

            "xor %%eax, %%eax\n\t"          // rax = 0 (success)
            // fall through to label 4

            "4:\n\t"
            "pop %%r15\n\t"
            "pop %%r14\n\t"
            "pop %%r13\n\t"
            "pop %%r12\n\t"
            "pop %%rbp\n\t"
            "pop %%rbx\n\t"

            : "=a"(result)
            : [regs] "r"(regs), [launch] "r"(launch_val)
            : "rcx", "rdx", "rdi", "rsi",
              "r8", "r9", "r10", "r11", "cc", "memory"
        );

        if (result != 0) {
            uint64_t err_code = VMRead(VMCS_VM_INSTR_ERROR);
            last_vm_instr_error = (uint32_t)err_code;
            last_vm_entry_guest_rip = VMRead(VMCS_GUEST_RIP);
            last_vm_entry_guest_cr0 = VMRead(VMCS_GUEST_CR0);
            last_vm_entry_guest_cr4 = VMRead(VMCS_GUEST_CR4);
            SerialLogger::Log("VMM: VM entry failed, error=");
            SerialLogger::LogDec((int)err_code);
            SerialLogger::Log(" guest_rip=0x");
            SerialLogger::LogHex(last_vm_entry_guest_rip);
            SerialLogger::Log(" guest_cr0=0x");
            SerialLogger::LogHex(last_vm_entry_guest_cr0);
            SerialLogger::Log(" guest_cr4=0x");
            SerialLogger::LogHex(last_vm_entry_guest_cr4);
            SerialLogger::Log("\r\n");
            cpu->exit_reason = 0xFFFFFFFF;
            return -1;
        }

        // read vm-exit information from vmcs
        cpu->exit_reason = VMRead(VMCS_EXIT_REASON);
        cpu->exit_qualification = VMRead(VMCS_EXIT_QUALIFICATION);

        if (!cpu->launched) cpu->launched = true;
        return (int)cpu->exit_reason;

    } else if (cpu->type == VIRT_AMD_SVM) {
        // amd-v: must manually save/restore guest gprs around vmrun
        // vmcb auto-saves/restores rax only. all other gprs must be
        // loaded from cpu->regs[] before vmrun and saved back after.
        uint64_t phys = (uint64_t)(uintptr_t)cpu->vmcb;
        uint64_t* regs = cpu->regs;

        // write guest rax to vmcb (hardware loads it from there)
        cpu->vmcb->rax = regs[0];

        // set global pointer for asm exit path
        g_svm_guest_regs = regs;

        asm volatile(
            // save host callee-saved registers
            "push %%rbx\n\t"
            "push %%rbp\n\t"
            "push %%r12\n\t"
            "push %%r13\n\t"
            "push %%r14\n\t"
            "push %%r15\n\t"

            // copy regs pointer to rax (before we overwrite callee-saved regs)
            "mov %[regs], %%rax\n\t"

            // load guest gprs from cpu->regs[]
            "mov 8(%%rax),  %%rcx\n\t"      // regs[1] = rcx
            "mov 16(%%rax), %%rdx\n\t"      // regs[2] = rdx
            "mov 24(%%rax), %%rbx\n\t"      // regs[3] = rbx
            // regs[4] = rsp  -  in vmcb, skip
            "mov 40(%%rax), %%rbp\n\t"      // regs[5] = rbp
            "mov 48(%%rax), %%rsi\n\t"      // regs[6] = rsi  (boot_params!)
            "mov 56(%%rax), %%rdi\n\t"      // regs[7] = rdi
            "mov 64(%%rax), %%r8\n\t"
            "mov 72(%%rax), %%r9\n\t"
            "mov 80(%%rax), %%r10\n\t"
            "mov 88(%%rax), %%r11\n\t"
            "mov 96(%%rax), %%r12\n\t"
            "mov 104(%%rax),%%r13\n\t"
            "mov 112(%%rax),%%r14\n\t"
            "mov 120(%%rax),%%r15\n\t"

            // load vmcb physical address into rax and run guest
            "mov %[phys], %%rax\n\t"
            "vmrun\n\t"
            // guest rax is auto-saved to vmcb by hardware
            // all other gprs still hold guest values  -  save them

            "push %%rax\n\t"                // save guest rax (scratch)
            "movabs $g_svm_guest_regs, %%rax\n\t"
            "mov (%%rax), %%rax\n\t"         // rax = &regs[0]

            "mov %%rcx, 8(%%rax)\n\t"       // regs[1] = rcx
            "mov %%rdx, 16(%%rax)\n\t"      // regs[2] = rdx
            "mov %%rbx, 24(%%rax)\n\t"      // regs[3] = rbx
            "mov %%rbp, 40(%%rax)\n\t"      // regs[5] = rbp
            "mov %%rsi, 48(%%rax)\n\t"      // regs[6] = rsi
            "mov %%rdi, 56(%%rax)\n\t"      // regs[7] = rdi
            "mov %%r8,  64(%%rax)\n\t"
            "mov %%r9,  72(%%rax)\n\t"
            "mov %%r10, 80(%%rax)\n\t"
            "mov %%r11, 88(%%rax)\n\t"
            "mov %%r12, 96(%%rax)\n\t"
            "mov %%r13, 104(%%rax)\n\t"
            "mov %%r14, 112(%%rax)\n\t"
            "mov %%r15, 120(%%rax)\n\t"

            "pop %%rcx\n\t"                  // recover guest rax
            "mov %%rcx, 0(%%rax)\n\t"        // regs[0] = rax

            // restore host callee-saved registers
            "pop %%r15\n\t"
            "pop %%r14\n\t"
            "pop %%r13\n\t"
            "pop %%r12\n\t"
            "pop %%rbp\n\t"
            "pop %%rbx\n\t"
            :
            : [phys] "r"(phys), [regs] "r"(regs)
            : "rax", "rcx", "rdx", "rdi", "rsi",
              "r8", "r9", "r10", "r11", "cc", "memory"
        );

        // read back guest rax from vmcb (hardware saved it)
        regs[0] = cpu->vmcb->rax;

        cpu->launched = true;
        cpu->exit_reason = (uint32_t)cpu->vmcb->exit_code;
        cpu->exit_qualification = (uint32_t)cpu->vmcb->exit_info1;
        return (int)cpu->exit_reason;
    }
    return -1;
}

//  vmcs setup (intel vt-x)

void VMM::SetupVMCSControls(vCPU* cpu) {
    (void)cpu;

    // read allowed pin-based controls
    uint64_t pin_msr = ReadMSR(MSR_IA32_VMX_PINBASED_CTLS);
    uint32_t pin_allowed0 = (uint32_t)(pin_msr & 0xFFFFFFFF);
    uint32_t pin_allowed1 = (uint32_t)(pin_msr >> 32);
    uint32_t pin_controls = PIN_EXTERNAL_INT_EXIT | PIN_NMI_EXIT;
    pin_controls |= pin_allowed0;
    pin_controls &= pin_allowed1;
    VMWrite(VMCS_PIN_BASED_CONTROLS, pin_controls);

    // primary processor-based controls
    uint64_t proc_msr = ReadMSR(MSR_IA32_VMX_PROCBASED_CTLS);
    uint32_t proc_allowed0 = (uint32_t)(proc_msr & 0xFFFFFFFF);
    uint32_t proc_allowed1 = (uint32_t)(proc_msr >> 32);
    uint32_t proc_controls = PROC_HLT_EXIT | PROC_IO_EXIT | PROC_SECONDARY_CONTROLS
                           | PROC_MOV_DR_EXIT | PROC_MSR_BITMAPS;
    proc_controls |= proc_allowed0;
    proc_controls &= proc_allowed1;
    VMWrite(VMCS_PROC_BASED_CONTROLS, proc_controls);

    // secondary processor-based controls
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

    // vm-exit controls  -  must set host_addr_space_size (bit 9) for 64-bit host
    uint64_t exit_msr = ReadMSR(MSR_IA32_VMX_EXIT_CTLS);
    uint32_t exit_allowed0 = (uint32_t)(exit_msr & 0xFFFFFFFF);
    uint32_t exit_allowed1 = (uint32_t)(exit_msr >> 32);
    uint32_t exit_controls = VM_EXIT_HOST_ADDR_SPACE_SIZE; // 64-bit host
    exit_controls |= exit_allowed0;
    exit_controls &= exit_allowed1;
    VMWrite(VMCS_EXIT_CONTROLS, exit_controls);

    // vm-entry controls
    uint64_t entry_msr = ReadMSR(MSR_IA32_VMX_ENTRY_CTLS);
    uint32_t entry_allowed0 = (uint32_t)(entry_msr & 0xFFFFFFFF);
    uint32_t entry_allowed1 = (uint32_t)(entry_msr >> 32);
    uint32_t entry_controls = 0;
    entry_controls |= entry_allowed0;
    entry_controls &= entry_allowed1;
    VMWrite(VMCS_ENTRY_CONTROLS, entry_controls);

    // intercept only #db (1) and #bp (3) exceptions  -  let guest handle others
    VMWrite(VMCS_EXCEPTION_BITMAP, (1 << 1) | (1 << 3));

    // vmcs link pointer  -  must be all-ones when not using vmcs shadowing
    // this is a 64-bit vmcs field; a single vmwrite sets all 64 bits on a
    // 64-bit host.  do not write field+1  -  that targets a different encoding.
    VMWrite(VMCS_VMCS_LINK_PTR, 0xFFFFFFFFFFFFFFFFULL);

    // guest activity state = active (0)
    VMWrite(VMCS_GUEST_ACTIVITY, 0);
    VMWrite(VMCS_GUEST_INTERRUPTIBILITY, 0);
}

void VMM::SetupVMCSHostState(vCPU* cpu) {
    (void)cpu;

    // host cr0/cr3/cr4
    VMWrite(VMCS_HOST_CR0, ReadCR0());
    VMWrite(VMCS_HOST_CR3, ReadCR3());
    VMWrite(VMCS_HOST_CR4, ReadCR4());

    // host selectors
    uint16_t cs, ss, ds, es, fs, gs, tr;
    asm volatile("mov %%cs, %0" : "=r"(cs));
    asm volatile("mov %%ss, %0" : "=r"(ss));
    asm volatile("mov %%ds, %0" : "=r"(ds));
    asm volatile("mov %%es, %0" : "=r"(es));
    asm volatile("mov %%fs, %0" : "=r"(fs));
    asm volatile("mov %%gs, %0" : "=r"(gs));
    asm volatile("str %0"       : "=r"(tr));
    cs &= 0xF8;
    ss &= 0xF8;
    ds &= 0xF8;
    es &= 0xF8;
    fs &= 0xF8;
    gs &= 0xF8;
    tr &= 0xF8;

    VMWrite(VMCS_HOST_CS_SEL, cs);
    VMWrite(VMCS_HOST_SS_SEL, ss);
    VMWrite(VMCS_HOST_DS_SEL, ds);
    VMWrite(VMCS_HOST_ES_SEL, es);
    VMWrite(VMCS_HOST_FS_SEL, fs);
    VMWrite(VMCS_HOST_GS_SEL, gs);
    VMWrite(VMCS_HOST_TR_SEL, tr);

    PackedDescriptorTablePtr gdtr;
    PackedDescriptorTablePtr idtr;
    asm volatile("sgdt %0" : "=m"(gdtr));
    asm volatile("sidt %0" : "=m"(idtr));

    VMWrite(VMCS_HOST_FS_BASE, ReadFSBase());
    VMWrite(VMCS_HOST_GS_BASE, ReadGSBase());
    VMWrite(VMCS_HOST_TR_BASE, ReadTRBase(tr));
    VMWrite(VMCS_HOST_GDTR_BASE, gdtr.base);
    VMWrite(VMCS_HOST_IDTR_BASE, idtr.base);
    VMWrite(VMCS_HOST_IA32_SYSENTER_CS, ReadMSR(0x174) & 0xFFFF);
    VMWrite(VMCS_HOST_IA32_SYSENTER_ESP, ReadMSR(0x175));
    VMWrite(VMCS_HOST_IA32_SYSENTER_EIP, ReadMSR(0x176));

    // host rip  -  point to the vm-exit return stub
    // host rsp  -  current stack pointer
    uint64_t rsp_val;
    asm volatile("mov %%rsp, %0" : "=r"(rsp_val));
    VMWrite(VMCS_HOST_RSP, rsp_val);
    VMWrite(VMCS_HOST_RIP, (uint64_t)(uintptr_t)&_vmm_host_return_stub);
}

void VMM::SetupVMCSGuestState(vCPU* cpu) {
    (void)cpu;

    // basic guest state: real mode at 0x7c00 (like bios boot)
    // cr0: et=1, ne=1, pe=0 (real mode).  must satisfy vmx fixed-bit
    // requirements from msr_ia32_vmx_cr0_fixed0/fixed1, but with unrestricted
    // guest enabled bits 0 (pe) and 31 (pg) are exempt.
    {
        uint64_t cr0 = 0x00000030; // et + ne, pe=0 (real mode)
        uint64_t fixed0 = 0, fixed1 = ~0ULL;
        SafeReadMSR(MSR_IA32_VMX_CR0_FIXED0, fixed0);
        SafeReadMSR(MSR_IA32_VMX_CR0_FIXED1, fixed1);
        // unrestricted guest relaxes pe and pg constraints
        fixed0 &= ~((1ULL << 0) | (1ULL << 31));
        fixed1 |=  ((1ULL << 0) | (1ULL << 31));
        cr0 |= fixed0;
        cr0 &= fixed1;
        VMWrite(VMCS_GUEST_CR0, cr0);
        VMWrite(VMCS_CR0_READ_SHADOW, 0x00000030); // guest sees original value
    }
    VMWrite(VMCS_GUEST_CR3, 0);
    VMWrite(VMCS_GUEST_CR4, 0);

    // guest selectors  -  real mode
    VMWrite(VMCS_GUEST_CS_SEL, 0x0000);
    VMWrite(VMCS_GUEST_CS_BASE, 0x00000000);
    VMWrite(VMCS_GUEST_CS_LIMIT, 0xFFFF);
    VMWrite(VMCS_GUEST_CS_AR, 0x009B);  // present, dpl0, code, r/x

    VMWrite(VMCS_GUEST_SS_SEL, 0x0000);
    VMWrite(VMCS_GUEST_SS_BASE, 0x00000000);
    VMWrite(VMCS_GUEST_SS_LIMIT, 0xFFFF);
    VMWrite(VMCS_GUEST_SS_AR, 0x0093);  // present, dpl0, data, r/w

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
    VMWrite(VMCS_GUEST_FS_LIMIT, 0);
    VMWrite(VMCS_GUEST_FS_AR, 0x10000); // unusable null segment

    VMWrite(VMCS_GUEST_GS_SEL, 0x0000);
    VMWrite(VMCS_GUEST_GS_BASE, 0x00000000);
    VMWrite(VMCS_GUEST_GS_LIMIT, 0);
    VMWrite(VMCS_GUEST_GS_AR, 0x10000); // unusable null segment

    // tr (task register)  -  required to be present in vmcs
    VMWrite(VMCS_GUEST_TR_SEL, 0x0008);
    VMWrite(VMCS_GUEST_TR_BASE, 0x00000000);
    VMWrite(VMCS_GUEST_TR_LIMIT, 0x0067);
    VMWrite(VMCS_GUEST_TR_AR, 0x008B); // 32-bit busy tss, present

    // ldtr
    VMWrite(VMCS_GUEST_LDTR_SEL, 0x0000);
    VMWrite(VMCS_GUEST_LDTR_BASE, 0x00000000);
    VMWrite(VMCS_GUEST_LDTR_LIMIT, 0xFFFF);
    VMWrite(VMCS_GUEST_LDTR_AR, 0x10000); // unusable (bit 16)

    // gdtr/idtr
    VMWrite(VMCS_GUEST_GDTR_BASE, 0);
    VMWrite(VMCS_GUEST_GDTR_LIMIT, 0xFFFF);
    VMWrite(VMCS_GUEST_IDTR_BASE, 0);
    VMWrite(VMCS_GUEST_IDTR_LIMIT, 0x3FF); // real mode ivt

    // guest rip = 0x7c00 (boot sector), rsp = 0x7000, rflags = 0x02
    VMWrite(VMCS_GUEST_RIP, 0x7C00);
    VMWrite(VMCS_GUEST_RSP, 0x7000);
    VMWrite(VMCS_GUEST_RFLAGS, 0x00000002);  // bit 1 always set
}

//  vmcb setup (amd-v / svm)

void VMM::SetupVMCBControls(vCPU* cpu) {
    if (!cpu || !cpu->vmcb) return;
    VMCB* vmcb = cpu->vmcb;

    // intercept important operations
    // with nested paging enabled, we don't need to intercept cr reads/writes
    // (the hardware handles them via npt). only intercept what we must.
    vmcb->intercept_cr_read  = 0;  // no cr read intercepts (npt handles)
    vmcb->intercept_cr_write = 0;  // no cr write intercepts (npt handles)

    vmcb->intercept_misc1 = SVM_INTERCEPT_CPUID | SVM_INTERCEPT_HLT
                          | SVM_INTERCEPT_IOIO | SVM_INTERCEPT_MSR;

    // intercept_misc2: vmmcall must be intercepted for vmcall bridge.
    // vmrun must be intercepted (required by amd spec for nested safety).
    // don't intercept intr/nmi  -  let guest handle them directly.
    vmcb->intercept_misc2 = SVM_INTERCEPT2_VMRUN | SVM_INTERCEPT2_VMMCALL;

    // guest asid (must be non-zero)
    vmcb->guest_asid = 1;

    // enable nested paging
    vmcb->np_enable = 1;

    // tlb control: flush all on first entry
    vmcb->tlb_control = 1;
}

void VMM::SetupVMCBGuestState(vCPU* cpu) {
    if (!cpu || !cpu->vmcb) return;
    VMCB* vmcb = cpu->vmcb;

    // real mode  -  start at 0x7c00
    vmcb->cs.selector = 0x0000;
    vmcb->cs.base     = 0x00000000;
    vmcb->cs.limit    = 0xFFFF;
    vmcb->cs.attrib   = 0x049B;  // present, read/execute, dpl=0

    vmcb->ss.selector = 0x0000;
    vmcb->ss.base     = 0x00000000;
    vmcb->ss.limit    = 0xFFFF;
    vmcb->ss.attrib   = 0x0493;

    vmcb->ds = vmcb->ss;
    vmcb->es = vmcb->ss;
    vmcb->fs = vmcb->ss;
    vmcb->gs = vmcb->ss;

    // gdt and idt  -  minimal real-mode
    vmcb->gdtr.base  = 0;
    vmcb->gdtr.limit = 0xFFFF;
    vmcb->idtr.base  = 0;
    vmcb->idtr.limit = 0x3FF;

    // control registers
    vmcb->cr0    = 0x00000030;  // et=1, nw=1 (no pe)
    vmcb->cr3    = 0;
    vmcb->cr4    = 0;
    vmcb->efer   = (1ULL << 12); // svme must be set
    vmcb->dr6    = 0xFFFF0FF0;
    vmcb->dr7    = 0x00000400;

    // execution state
    vmcb->rip    = 0x7C00;
    vmcb->rsp    = 0x7000;
    vmcb->rax    = 0;
    vmcb->rflags = 0x00000002;

    // pat  -  default value
    vmcb->g_pat  = 0x0007040600070406ULL;
}

//  debug / status

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
