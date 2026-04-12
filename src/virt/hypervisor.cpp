//  kurono os  -  hypervisor / vm lifecycle manager implementation
//  orchestrates all virtualization components to create and run a vm
//  capable of booting a linux kernel.
//
//  architecture:
//    hypervisor::init()
//      → detect hardware (vt-x / amd-v)
//      → initialize vmm
//    hypervisor::createvm(config)
//      → allocate guest ram (guestmemorymanager)
//      → create vcpu, ept/npt
//      → initialize virtual devices (pic, apic, pit, hpet, serial, disk)
//      → set up i/o bitmap, msr bitmap
//    hypervisor::loadlinuxkernel(bzimage, cmdline)
//      → parse bzimage header (linuxbootloader)
//      → load setup code + kernel into guest memory
//      → fill boot_params, e820, command line
//    hypervisor::runvm()
//      → configure vmcs/vmcb for protected-mode entry
//      → vm-entry → vm-exit → handle → re-enter loop
//      → tick virtual devices periodically
//      → inject interrupts when guest is interruptible
//
//  reference: intel sdm vol 3c chapters 23-33, amd apm vol 2 chapter 15
#include "hypervisor.h"
#include "../kernel/types.h"
#include "../kernel/heap.h"
#include "../drivers/serial.h"
#include "../linux/linux_drivers.h"
#include "../shell/shell.h"

// helper: allocate aligned memory from kernel heap
static void* HVAllocAligned(size_t size, size_t align) {
    void* raw = KernelHeap::Alloc(size + align + sizeof(void*));
    if (!raw) return nullptr;
    uintptr_t addr = (uintptr_t)raw + sizeof(void*);
    uintptr_t aligned = (addr + align - 1) & ~(align - 1);
    ((void**)aligned)[-1] = raw;
    return (void*)aligned;
}

static void HVFreeAligned(void* aligned) {
    if (!aligned) return;
    void* raw = ((void**)aligned)[-1];
    KernelHeap::Free(raw);
}

static void _auto_setup_alpine_userland();

VMState      Hypervisor::vm_state      = VM_STATE_UNINITIALIZED;
VMConfig     Hypervisor::config;
vCPU*        Hypervisor::vcpu          = nullptr;
bool         Hypervisor::hw_available  = false;
EPT_PML4*    Hypervisor::ept_root      = nullptr;
NPT_PML4*    Hypervisor::npt_root      = nullptr;
VirtualSerial Hypervisor::serial;
VirtualDisk  Hypervisor::disk;
uint8_t*     Hypervisor::io_bitmap_a   = nullptr;
uint8_t*     Hypervisor::io_bitmap_b   = nullptr;
uint8_t*     Hypervisor::msr_bitmap    = nullptr;
VMStats      Hypervisor::stats;
bool         Hypervisor::alpine_booted     = false;
char         Hypervisor::alpine_boot_log[8192];
int          Hypervisor::alpine_boot_log_len = 0;
bool         Hypervisor::debian_booted     = false;
char         Hypervisor::debian_boot_log[8192];
int          Hypervisor::debian_boot_log_len = 0;
bool         Hypervisor::linux_guest_enabled = true;
LinuxGuestProfile Hypervisor::linux_guest_profile = LINUX_GUEST_ALPINE;

static void AppendDiagText(char* buf, int& len, int max, const char* text) {
    if (!buf || !text || max <= 0) return;
    while (*text && len < max - 1) {
        buf[len++] = *text++;
    }
    buf[len] = 0;
}

static void AppendDiagDec(char* buf, int& len, int max, uint32_t value) {
    char tmp[16];
    int n = 0;
    if (value == 0) {
        AppendDiagText(buf, len, max, "0");
        return;
    }
    while (value && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (n > 0 && len < max - 1) {
        buf[len++] = tmp[--n];
    }
    buf[len] = 0;
}

static void AppendDiagHex(char* buf, int& len, int max, uint64_t value) {
    static const char* hex = "0123456789ABCDEF";
    AppendDiagText(buf, len, max, "0x");
    bool started = false;
    for (int shift = 60; shift >= 0; shift -= 4) {
        uint8_t nibble = (uint8_t)((value >> shift) & 0xFULL);
        if (!started && nibble == 0 && shift > 0) continue;
        started = true;
        if (len < max - 1) buf[len++] = hex[nibble];
    }
    if (!started && len < max - 1) buf[len++] = '0';
    buf[len] = 0;
}

static void FillGenericVMEntryFailureDiag(char* buf, int& len, int max, const char* guest_name) {
    AppendDiagText(buf, len, max, "[kurono] ");
    AppendDiagText(buf, len, max, guest_name);
    AppendDiagText(buf, len, max, " boot failed before guest serial output.\n");

    if (VMM::IsWHPX() && VMM::IsNested() && !VMM::IsWHPXNestedOk()) {
        AppendDiagText(buf, len, max, "[kurono] VT-x/AMD-V is visible, but the outer Hyper-V/WHPX host\n");
        AppendDiagText(buf, len, max, "[kurono] is not exposing nested virtualization to Kurono.\n");
        AppendDiagText(buf, len, max, "[kurono] This is a host limitation, not a guest boot bug.\n");
        return;
    }

    if (VMM::GetType() == VIRT_INTEL_VTX) {
        AppendDiagText(buf, len, max, "[kurono] Backend: Intel VT-x on this machine.\n");
        AppendDiagText(buf, len, max, "[kurono] VM-entry failed on the local VT-x path; this is not the old\n");
        AppendDiagText(buf, len, max, "[kurono] WHPX/QEMU nested-virtualization fallback message.\n");
        AppendDiagText(buf, len, max, "[kurono] VM-instruction error: ");
        AppendDiagDec(buf, len, max, VMM::GetLastVMInstructionError());
        AppendDiagText(buf, len, max, "\n[kurono] Guest RIP: ");
        AppendDiagHex(buf, len, max, VMM::GetLastVMEntryGuestRip());
        AppendDiagText(buf, len, max, "\n[kurono] Guest CR0: ");
        AppendDiagHex(buf, len, max, VMM::GetLastVMEntryGuestCr0());
        AppendDiagText(buf, len, max, "\n[kurono] Guest CR4: ");
        AppendDiagHex(buf, len, max, VMM::GetLastVMEntryGuestCr4());
        AppendDiagText(buf, len, max, "\n[kurono] Inspect the serial log for the precise VMX failure context.\n");
        return;
    }

    if (VMM::GetType() == VIRT_AMD_SVM) {
        AppendDiagText(buf, len, max, "[kurono] Backend: AMD-V on this machine.\n");
        AppendDiagText(buf, len, max, "[kurono] Guest execution failed before serial output. Review the serial\n");
        AppendDiagText(buf, len, max, "[kurono] log for the exact SVM/NPT failure context.\n");
        return;
    }

    AppendDiagText(buf, len, max, "[kurono] Hardware virtualization is not available to the guest hypervisor.\n");
}

//  init  -  detect hardware virtualization

bool Hypervisor::Init() {
    SerialLogger::Log("Hypervisor: Initializing...\r\n");

    // initialize the vmm (hardware detection + whpx identification)
    VMM::Init();
    hw_available = VMM::IsSupported();

    if (hw_available) {
        SerialLogger::Log("Hypervisor: Hardware virtualization available (");
        SerialLogger::Log(VMM::GetType() == VIRT_INTEL_VTX ? "Intel VT-x" : "AMD-V");
        if (VMM::IsWHPX()) SerialLogger::Log(", via WHPX");
        if (VMM::IsNested()) SerialLogger::Log(", nested");
        SerialLogger::Log(")\r\n");
    } else {
        SerialLogger::Log("Hypervisor: No hardware virtualization");
        if (VMM::IsWHPX()) {
            SerialLogger::Log("  -  WHPX detected but nested virt blocked by host");
        }
        SerialLogger::Log("  -  software emulation only\r\n");
    }

    // initialize exit handler
    VMExitHandler::Init();

    // initialize ept manager
    EPTManager::Init();

    // initialize virtual devices
    VirtualDevices::Init();

    ResetStats();
    vm_state = VM_STATE_UNINITIALIZED;

    SerialLogger::Log("Hypervisor: Init complete\r\n");
    return true;
}

bool Hypervisor::IsAvailable() {
    return hw_available;
}

//  createvm  -  set up a new virtual machine

bool Hypervisor::CreateVM(const VMConfig& cfg) {
    if (vm_state != VM_STATE_UNINITIALIZED && vm_state != VM_STATE_DESTROYED) {
        SerialLogger::Log("Hypervisor: VM already exists\r\n");
        return false;
    }

    config = cfg;
    SerialLogger::Log("Hypervisor: Creating VM (RAM=");
    SerialLogger::LogDec(config.ram_mb);
    SerialLogger::Log("MB, Disk=");
    SerialLogger::LogDec(config.disk_size_mb);
    SerialLogger::Log("MB)\r\n");

    GuestMemoryManager::Init(config.ram_mb);
    if (!GuestMemoryManager::GetLowRAM()) {
        SerialLogger::Log("Hypervisor: Failed to allocate guest memory\r\n");
        return false;
    }

    if (hw_available) {
        vcpu = VMM::CreateVCPU();
        if (!vcpu) {
            SerialLogger::Log("Hypervisor: Failed to create vCPU\r\n");
            GuestMemoryManager::FreeGuestRAM();
            return false;
        }
    } else {
        // software emulation: create a dummy vcpu
        vcpu = (vCPU*)KernelHeap::Alloc(sizeof(vCPU));
        if (vcpu) {
            memset(vcpu, 0, sizeof(vCPU));
            vcpu->type = VIRT_NONE;
        }
    }

    if (hw_available) {
        if (!SetupEPT()) {
            SerialLogger::Log("Hypervisor: EPT setup failed\r\n");
        }
    }

    if (hw_available) {
        SetupIOBitmap();
        SetupMSRBitmap();
    }

    SetupDevices();

    vm_state = VM_STATE_CREATED;
    SerialLogger::Log("Hypervisor: VM created successfully\r\n");
    return true;
}

//  destroyvm  -  tear down and free all vm resources

void Hypervisor::DestroyVM() {
    SerialLogger::Log("Hypervisor: Destroying VM...\r\n");

    if (vcpu) {
        if (hw_available && vcpu->type != VIRT_NONE) {
            VMM::DestroyVCPU(vcpu);
        } else {
            KernelHeap::Free(vcpu);
        }
        vcpu = nullptr;
    }

    if (ept_root) {
        EPTManager::DestroyEPT(ept_root);
        ept_root = nullptr;
    }
    if (npt_root) {
        EPTManager::DestroyNPT(npt_root);
        npt_root = nullptr;
    }

    if (io_bitmap_a)  { HVFreeAligned(io_bitmap_a); io_bitmap_a = nullptr; }
    if (io_bitmap_b)  { HVFreeAligned(io_bitmap_b); io_bitmap_b = nullptr; }
    if (msr_bitmap)   { HVFreeAligned(msr_bitmap); msr_bitmap = nullptr; }

    GuestMemoryManager::FreeGuestRAM();

    alpine_booted = false;
    debian_booted = false;
    vm_state = VM_STATE_DESTROYED;
    SerialLogger::Log("Hypervisor: VM destroyed\r\n");
}

void Hypervisor::SetLinuxGuestEnabled(bool enabled) {
    linux_guest_enabled = enabled;
    if (!enabled && vm_state != VM_STATE_UNINITIALIZED && vm_state != VM_STATE_DESTROYED) {
        DestroyVM();
    }
}

bool Hypervisor::IsLinuxGuestEnabled() { return linux_guest_enabled; }

bool Hypervisor::SetLinuxGuestProfile(LinuxGuestProfile profile) {
    if (linux_guest_profile == profile) return true;
    if (!CanSwitchLinuxGuestProfile()) return false;
    linux_guest_profile = profile;
    KuronoShell::alpine_cmd_cached = false;
    return true;
}

LinuxGuestProfile Hypervisor::GetLinuxGuestProfile() { return linux_guest_profile; }

const char* Hypervisor::GetLinuxGuestProfileName() {
    return linux_guest_profile == LINUX_GUEST_DEBIAN ? "Debian" : "Alpine";
}

bool Hypervisor::CanSwitchLinuxGuestProfile() {
    return vm_state == VM_STATE_UNINITIALIZED || vm_state == VM_STATE_DESTROYED;
}

//  setupiobitmap  -  configure which i/o ports cause vm-exits

bool Hypervisor::SetupIOBitmap() {
    // allocate two 4 kb pages (must be page-aligned)
    io_bitmap_a = (uint8_t*)HVAllocAligned(IO_BITMAP_A_SIZE, 4096);
    io_bitmap_b = (uint8_t*)HVAllocAligned(IO_BITMAP_B_SIZE, 4096);
    if (!io_bitmap_a || !io_bitmap_b) {
        SerialLogger::Log("Hypervisor: Failed to alloc IO bitmaps\r\n");
        return false;
    }

    // set all bits = 1 → all i/o ports cause vm-exit
    // this gives us full control but we can selectively allow later
    memset(io_bitmap_a, 0xFF, IO_BITMAP_A_SIZE);
    memset(io_bitmap_b, 0xFF, IO_BITMAP_B_SIZE);

    // if we want to pass through certain ports, clear their bits:
    // for example, pass through debug port 0xe9 (qemu debug console)
    // port 0xe9 is in bitmap a (port < 0x8000)
    // byte index = port / 8, bit = port % 8
    // io_bitmap_a[0xe9 / 8] &= ~(1 << (0xe9 % 8));

    // write bitmap addresses to vmcs if intel vmx
    if (VMM::GetType() == VIRT_INTEL_VTX && vcpu && vcpu->vmcs) {
        VMM::VMPtrLoad(vcpu->vmcs);
        VMM::VMWrite(VMCS_IO_BITMAP_A, (uint64_t)(uintptr_t)io_bitmap_a);
        VMM::VMWrite(VMCS_IO_BITMAP_B, (uint64_t)(uintptr_t)io_bitmap_b);
    }

    // amd svm: allocate a contiguous 12 kb iopm and set vmcb field
    // svm requires a single 12 kb region (3 pages) for the i/o permission map
    if (VMM::GetType() == VIRT_AMD_SVM && vcpu && vcpu->vmcb) {
        uint8_t* svm_iopm = (uint8_t*)HVAllocAligned(12288, 4096);
        if (svm_iopm) {
            memset(svm_iopm, 0xFF, 12288);  // intercept all i/o ports
            vcpu->vmcb->iopm_base_pa = (uint64_t)(uintptr_t)svm_iopm;
            SerialLogger::Log("Hypervisor: SVM IOPM (12KB) at ");
            SerialLogger::LogHex((uint32_t)(uintptr_t)svm_iopm);
            SerialLogger::Log("\r\n");
        } else {
            SerialLogger::Log("Hypervisor: Failed to alloc SVM IOPM\r\n");
        }
    }

    SerialLogger::Log("Hypervisor: I/O bitmap configured (intercept all)\r\n");
    return true;
}

//  setupmsrbitmap  -  configure which msrs cause vm-exits

bool Hypervisor::SetupMSRBitmap() {
    msr_bitmap = (uint8_t*)HVAllocAligned(MSR_BITMAP_SIZE, 4096);
    if (!msr_bitmap) {
        SerialLogger::Log("Hypervisor: Failed to alloc MSR bitmap\r\n");
        return false;
    }

    // layout of msr bitmap (4096 bytes):
    //   bytes 0x000-0x3ff: read bitmap for msrs 0x00000000-0x00001fff
    //   bytes 0x400-0x7ff: read bitmap for msrs 0xc0000000-0xc0001fff
    //   bytes 0x800-0xbff: write bitmap for msrs 0x00000000-0x00001fff
    //   bytes 0xc00-0xfff: write bitmap for msrs 0xc0000000-0xc0001fff

    // start with all msrs intercepted
    memset(msr_bitmap, 0xFF, MSR_BITMAP_SIZE);

    // allow safe msrs to pass through (no vm-exit):
    // ia32_time_stamp_counter (0x10)  -  let guest read tsc directly
    uint32_t tsc_byte = 0x10 / 8;
    uint32_t tsc_bit  = 0x10 % 8;
    msr_bitmap[tsc_byte] &= ~(1 << tsc_bit);               // read pass-through
    msr_bitmap[0x800 + tsc_byte] &= ~(1 << tsc_bit);       // write pass-through

    // write msr bitmap address to vmcs
    if (VMM::GetType() == VIRT_INTEL_VTX && vcpu && vcpu->vmcs) {
        VMM::VMPtrLoad(vcpu->vmcs);
        VMM::VMWrite(VMCS_MSR_BITMAP_ADDR, (uint64_t)(uintptr_t)msr_bitmap);
    }

    // amd svm: allocate an 8 kb msrpm and set vmcb field
    // svm requires 8 kb (2 pages) contiguous for the msr permission map
    if (VMM::GetType() == VIRT_AMD_SVM && vcpu && vcpu->vmcb) {
        uint8_t* svm_msrpm = (uint8_t*)HVAllocAligned(8192, 4096);
        if (svm_msrpm) {
            memset(svm_msrpm, 0xFF, 8192);  // intercept all msrs
            vcpu->vmcb->msrpm_base_pa = (uint64_t)(uintptr_t)svm_msrpm;
            SerialLogger::Log("Hypervisor: SVM MSRPM (8KB) at ");
            SerialLogger::LogHex((uint32_t)(uintptr_t)svm_msrpm);
            SerialLogger::Log("\r\n");
        } else {
            SerialLogger::Log("Hypervisor: Failed to alloc SVM MSRPM\r\n");
        }
    }

    SerialLogger::Log("Hypervisor: MSR bitmap configured\r\n");
    return true;
}

//  setupept  -  create extended page tables mapping guest physical memory

bool Hypervisor::SetupEPT() {
    if (VMM::GetType() == VIRT_INTEL_VTX) {
        ept_root = EPTManager::CreateEPT();
        if (!ept_root) return false;

        // map low ram (0 - 640 kb) = identity map to host allocation
        uint8_t* low_ram = GuestMemoryManager::GetLowRAM();
        uint32_t low_size = GuestMemoryManager::GetLowRAMSize();
        if (low_ram && low_size > 0) {
            EPTManager::MapRAM(ept_root, 0x00000000ULL,
                               (uint64_t)(uintptr_t)low_ram, low_size);
            EPTManager::AddRegion({0x00000000ULL,
                                   (uint64_t)(uintptr_t)low_ram,
                                   low_size, MEM_RAM, true, true, true});
        }

        // map vga buffer (0xa0000 - 0xbffff)
        uint8_t* vga = GuestMemoryManager::GetVGABuffer();
        if (vga) {
            EPTManager::MapMMIO(ept_root, GUEST_VGA_BASE,
                                (uint64_t)(uintptr_t)vga, (uint32_t)GUEST_VGA_SIZE);
        }

        // map rom area (0xc0000 - 0xfffff)
        uint8_t* rom = GuestMemoryManager::GetROMArea();
        if (rom) {
            EPTManager::MapROM(ept_root, GUEST_ROM_BASE,
                               (uint64_t)(uintptr_t)rom, (uint32_t)GUEST_ROM_SIZE);
        }

        // map high ram (1 mb+)
        uint8_t* high_ram = GuestMemoryManager::GetHighRAM();
        uint32_t high_size = GuestMemoryManager::GetHighRAMSize();
        if (high_ram && high_size > 0) {
            EPTManager::MapRAM(ept_root, GUEST_HIGH_RAM_START,
                               (uint64_t)(uintptr_t)high_ram, high_size);
            EPTManager::AddRegion({GUEST_HIGH_RAM_START,
                                   (uint64_t)(uintptr_t)high_ram,
                                   high_size, MEM_RAM, true, true, true});
        }

        // map lapic mmio (0xfee00000)
        EPTManager::AddRegion({GUEST_LAPIC_BASE, 0, 0x1000, MEM_MMIO,
                               true, true, false});

        // map hpet mmio (0xfed00000)
        EPTManager::AddRegion({GUEST_HPET_BASE, 0, 0x400, MEM_MMIO,
                               true, true, false});

        // write eptp to vmcs  -  single 64-bit write, do not split into
        // field / field+1 (field+1 is a different vmcs encoding on 64-bit)
        uint64_t eptp = EPTManager::BuildEPTP(ept_root);
        if (vcpu && vcpu->vmcs) {
            VMM::VMPtrLoad(vcpu->vmcs);
            VMM::VMWrite(VMCS_EPT_POINTER, eptp);
        }

        SerialLogger::Log("Hypervisor: EPT configured\r\n");

    } else if (VMM::GetType() == VIRT_AMD_SVM) {
        npt_root = EPTManager::CreateNPT();
        if (!npt_root) return false;

        // same mappings but using npt
        uint8_t* low_ram = GuestMemoryManager::GetLowRAM();
        uint32_t low_size = GuestMemoryManager::GetLowRAMSize();
        if (low_ram) {
            EPTManager::MapGuestPhysicalNPT(npt_root, 0x00000000ULL,
                (uint64_t)(uintptr_t)low_ram, low_size,
                NPT_PRESENT | NPT_WRITE | NPT_USER);
        }

        uint8_t* high_ram = GuestMemoryManager::GetHighRAM();
        uint32_t high_size = GuestMemoryManager::GetHighRAMSize();
        if (high_ram) {
            EPTManager::MapGuestPhysicalNPT(npt_root, GUEST_HIGH_RAM_START,
                (uint64_t)(uintptr_t)high_ram, high_size,
                NPT_PRESENT | NPT_WRITE | NPT_USER);
        }

        // set ncr3 in vmcb
        if (vcpu && vcpu->vmcb) {
            vcpu->vmcb->n_cr3 = EPTManager::BuildNCR3(npt_root);
            vcpu->vmcb->np_enable = 1;
        }

        SerialLogger::Log("Hypervisor: NPT configured\r\n");
    }

    return true;
}

//  setupdevices  -  initialize all virtual devices

bool Hypervisor::SetupDevices() {
    // serial port (com1)
    if (config.enable_serial) {
        serial.Init(COM1_BASE, COM1_IRQ);
        SerialLogger::Log("Hypervisor: Virtual COM1 enabled\r\n");
    }

    // ide disk
    if (config.enable_disk && config.disk_size_mb > 0) {
        disk.Init(config.disk_size_mb * 1024 * 1024);
        SerialLogger::Log("Hypervisor: Virtual IDE disk enabled\r\n");
    }

    // pic/apic/pit/hpet already initialized by virtualdevices::init()

    return true;
}

//  loadlinuxkernel  -  load a bzimage into guest memory

bool Hypervisor::LoadLinuxKernel(const uint8_t* bzimage, uint32_t size,
                                  const char* cmdline) {
    if (vm_state != VM_STATE_CREATED) {
        SerialLogger::Log("Hypervisor: VM not in CREATED state\r\n");
        return false;
    }

    const char* cmd = cmdline ? cmdline : config.cmdline;

    if (!LinuxBootLoader::LoadKernel(bzimage, size, cmd)) {
        SerialLogger::Log("Hypervisor: Failed to load Linux kernel\r\n");
        return false;
    }

    if (!LinuxBootLoader::SetupBootParams()) {
        SerialLogger::Log("Hypervisor: Failed to setup boot params\r\n");
        return false;
    }

    LinuxBootLoader::DumpImageInfo();
    return true;
}

bool Hypervisor::LoadInitrd(const uint8_t* data, uint32_t size) {
    return LinuxBootLoader::LoadInitrd(data, size);
}

//  configureguestprotectedmode  -  set up vmcs/vmcb for 32-bit protected mode
//  this is how linux expects to be entered (per boot protocol ≥ 2.00):
//    - protected mode enabled (cr0.pe = 1)
//    - flat 4 gb code/data segments
//    - paging disabled (cr0.pg = 0)
//    - a20 gate enabled
//    - gdt loaded with flat segments
//    - cs = __boot_cs (selector with flat 4 gb code segment)
//    - ds = es = fs = gs = ss = __boot_ds (flat 4 gb data segment)
//    - eip = code32_start (normally 0x100000)
//    - esi = boot_params address (pointer to struct boot_params)
//    - interrupts disabled (eflags.if = 0)

void Hypervisor::ConfigureGuestProtectedMode(uint32_t entry_point,
                                              uint32_t boot_params_addr) {
    if (!vcpu) return;

    if (vcpu->type == VIRT_INTEL_VTX) {
        VMM::VMPtrLoad(vcpu->vmcs);

        // cr0.pe = 1, cr0.ne = 1, cr0.et = 1
        // must also satisfy vmx cr0 fixed-bit constraints.
        // with unrestricted guest, pe (bit 0) and pg (bit 31) are exempt.
        uint32_t cr0 = 0x00000031; // pe + et + ne
        {
            uint64_t fixed0 = 0, fixed1 = ~0ULL;
            VMM::SafeReadMSR(MSR_IA32_VMX_CR0_FIXED0, fixed0);
            VMM::SafeReadMSR(MSR_IA32_VMX_CR0_FIXED1, fixed1);
            // unrestricted guest relaxes pe and pg constraints
            fixed0 &= ~((1ULL << 0) | (1ULL << 31));
            fixed1 |=  ((1ULL << 0) | (1ULL << 31));
            cr0 |= (uint32_t)fixed0;
            cr0 &= (uint32_t)fixed1;
        }
        VMM::VMWrite(VMCS_GUEST_CR0, cr0);
        VMM::VMWrite(VMCS_CR0_READ_SHADOW, 0x00000031);
        VMM::VMWrite(VMCS_GUEST_CR3, 0);
        VMM::VMWrite(VMCS_GUEST_CR4, 0);

        // cs: selector 0x10, flat code segment, 32-bit, dpl 0
        VMM::VMWrite(VMCS_GUEST_CS_SEL, 0x0010);
        VMM::VMWrite(VMCS_GUEST_CS_BASE, 0x00000000);
        VMM::VMWrite(VMCS_GUEST_CS_LIMIT, 0xFFFFFFFF);
        VMM::VMWrite(VMCS_GUEST_CS_AR, 0xC09B); // g=1,d=1,p=1,dpl=0,code,r/x

        // ds/es/fs/gs/ss: selector 0x18, flat data segment, 32-bit, dpl 0
        uint32_t data_ar = 0xC093; // g=1,b=1,p=1,dpl=0,data,r/w
        VMM::VMWrite(VMCS_GUEST_SS_SEL, 0x0018);
        VMM::VMWrite(VMCS_GUEST_SS_BASE, 0x00000000);
        VMM::VMWrite(VMCS_GUEST_SS_LIMIT, 0xFFFFFFFF);
        VMM::VMWrite(VMCS_GUEST_SS_AR, data_ar);

        VMM::VMWrite(VMCS_GUEST_DS_SEL, 0x0018);
        VMM::VMWrite(VMCS_GUEST_DS_BASE, 0x00000000);
        VMM::VMWrite(VMCS_GUEST_DS_LIMIT, 0xFFFFFFFF);
        VMM::VMWrite(VMCS_GUEST_DS_AR, data_ar);

        VMM::VMWrite(VMCS_GUEST_ES_SEL, 0x0018);
        VMM::VMWrite(VMCS_GUEST_ES_BASE, 0x00000000);
        VMM::VMWrite(VMCS_GUEST_ES_LIMIT, 0xFFFFFFFF);
        VMM::VMWrite(VMCS_GUEST_ES_AR, data_ar);

        // fs and gs are null selectors at linux entry, so mark them unusable.
        VMM::VMWrite(VMCS_GUEST_FS_SEL, 0x0000);
        VMM::VMWrite(VMCS_GUEST_FS_BASE, 0x00000000);
        VMM::VMWrite(VMCS_GUEST_FS_LIMIT, 0);
        VMM::VMWrite(VMCS_GUEST_FS_AR, 0x10000);

        VMM::VMWrite(VMCS_GUEST_GS_SEL, 0x0000);
        VMM::VMWrite(VMCS_GUEST_GS_BASE, 0x00000000);
        VMM::VMWrite(VMCS_GUEST_GS_LIMIT, 0);
        VMM::VMWrite(VMCS_GUEST_GS_AR, 0x10000);

        // place a 4-entry gdt at guest physical 0x1000:
        //   entry 0: null descriptor
        //   entry 1 (0x08): 32-bit busy tss
        //   entry 2 (0x10): 32-bit code, base=0, limit=4gb, dpl=0
        //   entry 3 (0x18): 32-bit data, base=0, limit=4gb, dpl=0
        uint64_t gdt[4];
        gdt[0] = 0x0000000000000000ULL; // null
        gdt[1] = 0x00008B0020000067ULL; // busy 32-bit tss at 0x2000, limit 0x67
        gdt[2] = 0x00CF9B000000FFFFULL; // code: base=0,limit=4g,g=1,d=1,type=b
        gdt[3] = 0x00CF93000000FFFFULL; // data: base=0,limit=4g,g=1,b=1,type=3

        GuestMemoryManager::WriteGuestPhys(0x1000, gdt, sizeof(gdt));
        uint8_t guest_tss[104] = {0};
        GuestMemoryManager::WriteGuestPhys(0x2000, guest_tss, sizeof(guest_tss));

        VMM::VMWrite(VMCS_GUEST_GDTR_BASE, 0x00001000);
        VMM::VMWrite(VMCS_GUEST_GDTR_LIMIT, sizeof(gdt) - 1);

        VMM::VMWrite(VMCS_GUEST_IDTR_BASE, 0x00000000);
        VMM::VMWrite(VMCS_GUEST_IDTR_LIMIT, 0); // no idt entries

        // must have a valid tr even in protected mode without tasks
        VMM::VMWrite(VMCS_GUEST_TR_SEL, 0x0008);
        VMM::VMWrite(VMCS_GUEST_TR_BASE, 0x00002000);
        VMM::VMWrite(VMCS_GUEST_TR_LIMIT, 0x00000067);
        VMM::VMWrite(VMCS_GUEST_TR_AR, 0x008B); // 32-bit busy tss

        VMM::VMWrite(VMCS_GUEST_LDTR_SEL, 0x0000);
        VMM::VMWrite(VMCS_GUEST_LDTR_BASE, 0x00000000);
        VMM::VMWrite(VMCS_GUEST_LDTR_LIMIT, 0);
        VMM::VMWrite(VMCS_GUEST_LDTR_AR, 0x10000); // unusable null ldtr

        VMM::VMWrite(VMCS_GUEST_RIP, entry_point);
        VMM::VMWrite(VMCS_GUEST_RSP, 0x00000000); // linux doesn't use stack at entry
        VMM::VMWrite(VMCS_GUEST_RFLAGS, 0x00000002); // only reserved bit 1

        // esi = pointer to boot_params
        // this is passed via general-purpose registers, but vmcs doesn't
        // have direct gpr fields  -  they're managed via the vm-exit save area.
        // for vmlaunch, we set esi in the vcpu register array.
        vcpu->regs[6] = boot_params_addr; // esi

        VMM::VMWrite(VMCS_VMCS_LINK_PTR, 0xFFFFFFFFFFFFFFFFULL);

        VMM::VMWrite(VMCS_GUEST_ACTIVITY, 0); // active
        VMM::VMWrite(VMCS_GUEST_INTERRUPTIBILITY, 0); // no blocking

        SerialLogger::Log("Hypervisor: VMCS configured for protected-mode entry at 0x");
        SerialLogger::LogHex(entry_point);
        SerialLogger::Log(" ESI=0x");
        SerialLogger::LogHex(boot_params_addr);
        SerialLogger::Log("\r\n");

    } else if (vcpu->type == VIRT_AMD_SVM && vcpu->vmcb) {
        VMCB* vmcb = vcpu->vmcb;

        // cr0: pe + ne + et
        vmcb->cr0 = 0x00000031;
        vmcb->cr3 = 0;
        vmcb->cr4 = 0;
        vmcb->efer = (1ULL << 12); // svme

        // code segment: flat 32-bit, g=1, d=1
        // vmcb attrib: bits[7:0]=type+s+dpl+p, bits[11:8]=avl+l+d+g
        vmcb->cs.selector = 0x0010;
        vmcb->cs.base     = 0x00000000;
        vmcb->cs.limit    = 0xFFFFFFFF;
        vmcb->cs.attrib   = 0x0C9B; // g=1,d=1,l=0,avl=0,p=1,dpl=0,s=1,type=b

        // data segments: flat 32-bit, g=1, b=1
        vmcb->ss.selector = 0x0018;
        vmcb->ss.base     = 0x00000000;
        vmcb->ss.limit    = 0xFFFFFFFF;
        vmcb->ss.attrib   = 0x0C93; // g=1,b=1,p=1,dpl=0,s=1,type=3

        vmcb->ds = vmcb->ss;
        vmcb->es = vmcb->ss;
        vmcb->fs.selector = 0;
        vmcb->fs.base = 0;
        vmcb->fs.limit = 0xFFFF;
        vmcb->fs.attrib = 0x0093;
        vmcb->gs = vmcb->fs;

        // tr: 32-bit busy tss (required for protected mode)
        vmcb->tr.selector = 0x0008;
        vmcb->tr.base     = 0x00000000;
        vmcb->tr.limit    = 0x0000FFFF;
        vmcb->tr.attrib   = 0x008B; // p=1, type=b (32-bit busy tss)

        // ldtr: null
        vmcb->ldtr.selector = 0x0000;
        vmcb->ldtr.base     = 0x00000000;
        vmcb->ldtr.limit    = 0x0000FFFF;
        vmcb->ldtr.attrib   = 0x0082; // p=1, type=2 (ldt)

        // gdt (same as intel)
        uint64_t gdt[4];
        gdt[0] = 0;
        gdt[1] = 0;
        gdt[2] = 0x00CF9B000000FFFFULL;
        gdt[3] = 0x00CF93000000FFFFULL;
        GuestMemoryManager::WriteGuestPhys(0x1000, gdt, sizeof(gdt));

        vmcb->gdtr.base  = 0x1000;
        vmcb->gdtr.limit = sizeof(gdt) - 1;
        vmcb->idtr.base  = 0;
        vmcb->idtr.limit = 0;

        vmcb->rip    = entry_point;
        vmcb->rsp    = 0;
        vmcb->rax    = 0;
        vmcb->rflags = 0x00000002;

        vmcb->dr6 = 0xFFFF0FF0;
        vmcb->dr7 = 0x00000400;
        vmcb->g_pat = 0x0007040600070406ULL;

        vcpu->regs[6] = boot_params_addr; // esi

        SerialLogger::Log("Hypervisor: VMCB configured for protected-mode entry at 0x");
        SerialLogger::LogHex(entry_point);
        SerialLogger::Log(" ESI=0x");
        SerialLogger::LogHex(boot_params_addr);
        SerialLogger::Log("\r\n");
    }
}

//  runvm  -  main execution loop

VMState Hypervisor::RunVM(uint32_t max_exits) {
    if (vm_state != VM_STATE_CREATED) {
        SerialLogger::Log("Hypervisor: Cannot run  -  VM not ready\r\n");
        return vm_state;
    }

    // configure vmcs/vmcb controls (host state, exec controls, svm enable, etc.)
    // this is required for both intel vt-x (vmcs setup) and amd svm
    // (svmenable + vmcb controls: guest_asid, intercepts, etc.)
    if (hw_available && vcpu) {
        if (!VMM::SetupVCPU(vcpu)) {
            SerialLogger::Log("Hypervisor: vCPU setup failed\r\n");
            vm_state = VM_STATE_CRASHED;
            return vm_state;
        }
    }

    // now configure guest state for protected-mode linux entry
    // this must come after setupvcpu because setupvcpu sets real-mode defaults
    uint32_t entry = LinuxBootLoader::GetEntryPoint();
    if (entry == 0) entry = LINUX_KERNEL_ADDR;
    ConfigureGuestProtectedMode(entry, LINUX_BOOT_PARAMS_ADDR);

    vm_state = VM_STATE_RUNNING;
    SerialLogger::Log("Hypervisor: Entering VM run loop\r\n");

    // read tsc at start for time-based safety timeout
    uint32_t tsc_lo_start, tsc_hi_start;
    asm volatile("rdtsc" : "=a"(tsc_lo_start), "=d"(tsc_hi_start));
    uint64_t tsc_start = ((uint64_t)tsc_hi_start << 32) | tsc_lo_start;
    // ~3 seconds at 3 ghz  -  prevents the loop from blocking boot display
    const uint64_t MAX_TSC_DELTA = 9000000000ULL;

    uint32_t exit_count = 0;
    while (vm_state == VM_STATE_RUNNING) {
        vm_state = RunOneCycle();

        exit_count++;
        if (max_exits > 0 && exit_count >= max_exits) {
            SerialLogger::Log("Hypervisor: Max exits reached (");
            SerialLogger::LogDec(max_exits);
            SerialLogger::Log(")\r\n");
            break;
        }

        // time-based safety: bail after ~3 seconds so boot isn't blocked
        if (exit_count % 200 == 0) {
            uint32_t tsc_lo, tsc_hi;
            asm volatile("rdtsc" : "=a"(tsc_lo), "=d"(tsc_hi));
            uint64_t tsc_now = ((uint64_t)tsc_hi << 32) | tsc_lo;
            if (tsc_now - tsc_start > MAX_TSC_DELTA) {
                SerialLogger::Log("Hypervisor: Boot timeout (~3s), yielding\r\n");
                break;
            }
        }

        // periodic device ticking (every 100 exits)
        if (exit_count % 100 == 0) {
            TickDevices();
        }
    }

    SerialLogger::Log("Hypervisor: VM run loop ended, state=");
    SerialLogger::LogDec(vm_state);
    SerialLogger::Log(" after ");
    SerialLogger::LogDec(exit_count);
    SerialLogger::Log(" exits\r\n");

    return vm_state;
}

//  runonecycle  -  single vm-entry → vm-exit → handle cycle

VMState Hypervisor::RunOneCycle() {
    if (!vcpu) return VM_STATE_CRASHED;

    stats.run_cycles++;

    // check for pending interrupts to inject before re-entry
    CheckAndInjectPendingIRQs();

    if (!hw_available) {
        // no hardware virt  -  can't actually run guest
        // in production this would be a software interpreter
        if (stats.run_cycles <= 1) {
            SerialLogger::Log("Hypervisor: No HW virt, cannot execute guest\r\n");
        }
        return VM_STATE_HALTED;
    }

    // enter guest
    int exit_reason = VMM::RunVCPU(vcpu);
    if (exit_reason < 0) {
        // vm-entry failed  -  under whpx this can happen if nested virt
        // isn't truly supported despite cpuid detection succeeding
        if (stats.run_cycles <= 1) {
            SerialLogger::Log("Hypervisor: VM-entry failed (exit_reason=");
            SerialLogger::LogDec(exit_reason);
            SerialLogger::Log(")\r\n");
            if (VMM::IsWHPX()) {
                SerialLogger::Log("Hypervisor: WHPX nested VM-entry rejected  -  "
                                 "host likely doesn't support nested virt\r\n");
            } else if (VMM::GetType() == VIRT_INTEL_VTX) {
                SerialLogger::Log("Hypervisor: Bare-metal/local VT-x VM-entry failure: instr_error=");
                SerialLogger::LogDec((int)VMM::GetLastVMInstructionError());
                SerialLogger::Log(" guest_rip=0x");
                SerialLogger::LogHex(VMM::GetLastVMEntryGuestRip());
                SerialLogger::Log(" guest_cr0=0x");
                SerialLogger::LogHex(VMM::GetLastVMEntryGuestCr0());
                SerialLogger::Log(" guest_cr4=0x");
                SerialLogger::LogHex(VMM::GetLastVMEntryGuestCr4());
                SerialLogger::Log("\r\n");
            }
        }
        return VM_STATE_CRASHED;
    }

    return ProcessVMExit();
}

//  processvmexit  -  dispatch vm-exit to handlers

VMState Hypervisor::ProcessVMExit() {
    if (!vcpu) return VM_STATE_CRASHED;

    stats.total_exits++;

    VMExitAction action;
    if (vcpu->type == VIRT_INTEL_VTX) {
        action = VMExitHandler::HandleExit(vcpu);
    } else if (vcpu->type == VIRT_AMD_SVM) {
        action = VMExitHandler::HandleSVMExit(vcpu);
    } else {
        return VM_STATE_CRASHED;
    }

    switch (action) {
        case VMEXIT_CONTINUE:
        case VMEXIT_HANDLED:
            return VM_STATE_RUNNING;

        case VMEXIT_SHUTDOWN:
            SerialLogger::Log("Hypervisor: Guest shutdown\r\n");
            return VM_STATE_HALTED;

        case VMEXIT_FATAL:
            SerialLogger::Log("Hypervisor: Fatal VM-exit\r\n");
            VMM::DumpVCPUState(vcpu);
            VMExitHandler::DumpStats();
            return VM_STATE_CRASHED;

        case VMEXIT_REBOOT:
            SerialLogger::Log("Hypervisor: Guest reboot requested\r\n");
            return VM_STATE_REBOOTING;

        default:
            return VM_STATE_CRASHED;
    }
}

//  handleguestio  -  route guest i/o to virtual devices

bool Hypervisor::HandleGuestIO(uint16_t port, bool is_out, uint8_t size,
                                 uint32_t& value) {
    stats.io_exits++;

    // try virtual serial port (com1: 0x3f8-0x3ff)
    if (config.enable_serial && port >= COM1_BASE && port <= COM1_BASE + 7) {
        if (is_out) {
            serial.WritePort(port, (uint8_t)(value & 0xFF));
            stats.serial_bytes_tx++;
        } else {
            value = serial.ReadPort(port);
            stats.serial_bytes_rx++;
        }
        return true;
    }

    // try virtual ide disk (0x1f0-0x1f7, 0x3f6)
    if (config.enable_disk) {
        if ((port >= ATA_PRI_DATA && port <= ATA_PRI_STATUS) ||
            port == ATA_PRI_ALTSTAT) {
            if (is_out) {
                disk.WritePort(port, value, size);
            } else {
                value = disk.ReadPort(port, size);
            }
            return true;
        }
    }

    // try pic/pit/etc via virtualdevices
    if (VirtualDevices::HandlePortIO(port, is_out, size, value)) {
        return true;
    }

    // port 0x80  -  post code (ignore)
    if (port == 0x80) return true;

    // port 0xcf8/0xcfc  -  pci configuration (stub: return 0xffffffff)
    if (port == 0xCF8 || port == 0xCFC) {
        if (!is_out) value = 0xFFFFFFFF;
        return true;
    }

    // port 0x92 (system control port a  -  a20 gate)
    if (port == 0x92) {
        if (!is_out) value = 0x02; // a20 enabled
        return true;
    }

    // port 0x70/0x71  -  cmos/rtc
    if (port == 0x70 || port == 0x71) {
        if (!is_out) value = 0;
        return true;
    }

    // unhandled
    return false;
}

//  handleguestmmio  -  route mmio to virtual devices

bool Hypervisor::HandleGuestMMIO(uint64_t phys_addr, bool is_write,
                                   uint8_t size, uint32_t& value) {
    stats.mmio_exits++;
    return VirtualDevices::HandleMMIO(phys_addr, is_write, size, value);
}

//  interrupt injection  -  inject interrupts/exceptions into guest

bool Hypervisor::InjectInterrupt(uint8_t vector) {
    if (!vcpu) return false;

    if (vcpu->type == VIRT_INTEL_VTX) {
        VMM::VMPtrLoad(vcpu->vmcs);

        // check if guest is interruptible
        uint32_t interruptibility = VMM::VMRead(VMCS_GUEST_INTERRUPTIBILITY);
        uint32_t rflags = VMM::VMRead(VMCS_GUEST_RFLAGS);

        // guest must have if=1 and not be in sti/mov ss shadow
        if (!(rflags & (1 << 9)) || (interruptibility & 0x03)) {
            return false; // not interruptible
        }

        // write to vm-entry interruption-information field
        // bits [7:0] = vector, [10:8] = type (0=ext int), [11] = deliver error code
        // [31] = valid
        uint32_t int_info = vector | (0 << 8) | (1u << 31);
        VMM::VMWrite(VMCS_ENTRY_INT_INFO, int_info);
        stats.irq_injections++;
        return true;

    } else if (vcpu->type == VIRT_AMD_SVM && vcpu->vmcb) {
        // svm event injection via vmcb eventinj field
        // [7:0]=vector, [10:8]=type(0=ext int), [11]=push error, [31]=valid
        vcpu->vmcb->event_inject = vector | (0 << 8) | (1u << 31);
        stats.irq_injections++;
        return true;
    }

    return false;
}

bool Hypervisor::InjectException(uint8_t vector, bool has_error,
                                  uint32_t error_code) {
    if (!vcpu) return false;

    if (vcpu->type == VIRT_INTEL_VTX) {
        VMM::VMPtrLoad(vcpu->vmcs);

        uint32_t int_info = vector | (3 << 8) | (1u << 31); // type 3 = hw exception
        if (has_error) {
            int_info |= (1 << 11);
            VMM::VMWrite(VMCS_ENTRY_EXCEPTION_ERROR, error_code);
        }
        VMM::VMWrite(VMCS_ENTRY_INT_INFO, int_info);
        return true;

    } else if (vcpu->type == VIRT_AMD_SVM && vcpu->vmcb) {
        uint64_t event = vector | (3 << 8) | (1u << 31);
        if (has_error) {
            event |= (1 << 11);
            // amd svm: error code goes in bits [63:32] of event_inject
            event |= ((uint64_t)error_code << 32);
        }
        vcpu->vmcb->event_inject = event;
        return true;
    }

    return false;
}

void Hypervisor::CheckAndInjectPendingIRQs() {
    // check virtual devices for pending irqs
    int irq = VirtualDevices::GetPendingIRQ();
    if (irq >= 0) {
        InjectInterrupt((uint8_t)irq);
    }

    // check serial port irq
    if (config.enable_serial && serial.HasPendingIRQ()) {
        VirtualDevices::GetMasterPIC().SetIRQ(serial.GetIRQ(), true);
        VirtualDevices::GetMasterPIC().SetIRQ(serial.GetIRQ(), false);
        serial.ClearIRQ();
    }

    // check ide irq
    if (config.enable_disk && disk.HasPendingIRQ()) {
        VirtualDevices::GetSlavePIC().SetIRQ(6, true); // irq 14 = slave irq 6
        VirtualDevices::GetSlavePIC().SetIRQ(6, false);
        disk.ClearIRQ();
    }
}

//  tickdevices  -  advance virtual device emulation

void Hypervisor::TickDevices() {
    stats.tick_count++;
    uint32_t us = config.timer_tick_us;

    VirtualDevices::Tick(us);

    if (config.enable_serial) {
        serial.Tick(us);
    }
}

//  pause / resume

void Hypervisor::PauseVM() {
    if (vm_state == VM_STATE_RUNNING) {
        vm_state = VM_STATE_PAUSED;
        SerialLogger::Log("Hypervisor: VM paused\r\n");
    }
}

void Hypervisor::ResumeVM() {
    if (vm_state == VM_STATE_PAUSED) {
        vm_state = VM_STATE_RUNNING;
        SerialLogger::Log("Hypervisor: VM resumed\r\n");
    }
}

//  serial console

int Hypervisor::ReadSerialOutput(char* buf, int max) {
    return serial.ReadOutput(buf, max);
}

bool Hypervisor::HasSerialOutput() {
    return serial.HasOutput();
}

//  serial command bridge  -  host → guest via virtual com1

void Hypervisor::SendSerialCommand(const char* cmd) {
    if (!cmd || vm_state != VM_STATE_RUNNING) return;

    // inject the command text followed by a newline into the guest's
    // serial receive buffer.  the guest sees this as typed input on ttys0.
    serial.InjectRxString(cmd);
    serial.InjectRxByte('\n');
    stats.serial_bytes_rx += 1; // account for the newline
    // count injected bytes
    for (const char* p = cmd; *p; p++) stats.serial_bytes_rx++;

    SerialLogger::Log("Hypervisor: Serial cmd → guest: ");
    SerialLogger::Log(cmd);
    SerialLogger::Log("\r\n");
}

void Hypervisor::SendSerialData(const uint8_t* data, int len) {
    if (!data || len <= 0 || vm_state != VM_STATE_RUNNING) return;
    for (int i = 0; i < len; i++) {
        serial.InjectRxByte(data[i]);
    }
    stats.serial_bytes_rx += (uint32_t)len;
}

int Hypervisor::DrainSerialOutput(char* buf, int max, int max_cycles) {
    // run a few vm cycles to give the guest time to produce output,
    // then read whatever the guest wrote to the serial port.
    if (vm_state != VM_STATE_RUNNING || max_cycles <= 0) {
        return serial.ReadOutput(buf, max);
    }

    for (int i = 0; i < max_cycles; i++) {
        VMState st = RunOneCycle();
        if (st != VM_STATE_RUNNING) break;
        if (serial.HasOutput()) break; // got something
    }
    return serial.ReadOutput(buf, max);
}

//  bootalpine  -  one-call boot of embedded alpine linux guest vm

bool Hypervisor::BootAlpine() {
    if (!alpine_data_available()) {
        SerialLogger::Log("Hypervisor: Alpine kernel/initramfs not embedded\r\n");
        return false;
    }

    SerialLogger::Log("Hypervisor: Booting Alpine Linux guest...\r\n");
    SerialLogger::Log("  Kernel size:   ");
    SerialLogger::LogDec(alpine_kernel_size());
    SerialLogger::Log(" bytes\r\n");
    SerialLogger::Log("  Initramfs size:");
    SerialLogger::LogDec(alpine_initramfs_size());
    SerialLogger::Log(" bytes\r\n");

    VMConfig cfg;
    cfg.SetDefaults();
    cfg.ram_mb        = 128;     // alpine needs more than the default 16 mb
    cfg.disk_size_mb  = 16;      // ram-backed disk for rootfs
    cfg.enable_serial = true;    // console on ttys0
    cfg.enable_disk   = true;    // /dev/sda for rootfs
    cfg.enable_apic   = true;
    cfg.timer_tick_us = 1000;
    cfg.cmdline       = "console=ttyS0 earlyprintk=serial,ttyS0,115200 "
                        "root=/dev/ram0 rw init=/sbin/init nokaslr noapic "
                        "nosmp noacpi pci=off";

    if (!CreateVM(cfg)) {
        SerialLogger::Log("Hypervisor: Failed to create VM for Alpine\r\n");
        return false;
    }

    if (!LoadLinuxKernel(alpine_kernel_data(), alpine_kernel_size(),
                         cfg.cmdline)) {
        SerialLogger::Log("Hypervisor: Failed to load Alpine kernel\r\n");
        DestroyVM();
        return false;
    }

    if (!LoadInitrd(alpine_initramfs_data(), alpine_initramfs_size())) {
        SerialLogger::Log("Hypervisor: Failed to load Alpine initramfs\r\n");
        DestroyVM();
        return false;
    }

    SerialLogger::Log("Hypervisor: Alpine VM configured  -  entering run loop\r\n");
    VMState final_state = RunVM(0); // run until halt/crash

    SerialLogger::Log("Hypervisor: Alpine VM exited with state ");
    SerialLogger::LogDec(final_state);
    SerialLogger::Log("\r\n");

    return (final_state != VM_STATE_CRASHED);
}

//  bootalpinewithextraction  -  boot alpine, capture boot log, extract drivers

bool Hypervisor::BootAlpineWithExtraction(uint32_t max_boot_exits) {
    if (!alpine_data_available()) {
        SerialLogger::Log("Hypervisor: Alpine kernel/initramfs not embedded\r\n");
        return false;
    }

    // reset boot log
    alpine_boot_log_len = 0;
    alpine_boot_log[0] = 0;
    alpine_booted = false;

    SerialLogger::Log("Hypervisor: BootAlpineWithExtraction starting\r\n");

    if (vm_state != VM_STATE_UNINITIALIZED &&
        vm_state != VM_STATE_DESTROYED) {
        DestroyVM();
    }
    Init();
    // re-read hw_available  -  init() detects svm/vt-x and may update it
    hw_available = VMM::IsSupported();

    if (hw_available) {
        SerialLogger::Log("Hypervisor: Attempting HW-accelerated Alpine boot");
        if (VMM::IsWHPX()) SerialLogger::Log(" [WHPX]");
        if (VMM::IsNested()) SerialLogger::Log(" [nested]");
        SerialLogger::Log("\r\n");

        VMConfig cfg;
        cfg.SetDefaults();
        cfg.ram_mb        = 256;
        cfg.disk_size_mb  = 16;
        cfg.enable_serial = true;
        cfg.enable_disk   = true;
        cfg.enable_apic   = true;
        cfg.timer_tick_us = 10000;
        cfg.cmdline       = "console=ttyS0 earlyprintk=serial,ttyS0,115200 "
                            "root=/dev/ram0 rw init=/sbin/init nokaslr noapic "
                            "nosmp noacpi pci=off";

        bool hw_ok = false;
        if (CreateVM(cfg)) {
            if (LoadLinuxKernel(alpine_kernel_data(), alpine_kernel_size(),
                                cfg.cmdline)) {
                if (LoadInitrd(alpine_initramfs_data(), alpine_initramfs_size())) {
                    SerialLogger::Log("Hypervisor: Running Alpine boot (");
                    SerialLogger::LogDec(max_boot_exits);
                    SerialLogger::Log(" max exits)...\r\n");

                    VMState st = RunVM(max_boot_exits);

                    // bare-metal alpine boot can need substantially more
                    // exits than the initial probe window, especially on
                    // real vt-x with frequent timer/serial exits. if the vm
                    // is still alive but hasn't emitted serial yet, continue
                    // incrementally rather than treating that as failure.
                    uint32_t extra_budget = max_boot_exits;
                    if (extra_budget < 250000) extra_budget = 250000;
                    uint32_t used_budget = 0;
                    while ((st == VM_STATE_RUNNING || st == VM_STATE_PAUSED) &&
                           alpine_boot_log_len == 0 &&
                           used_budget < extra_budget) {
                        uint32_t step = 25000;
                        if (extra_budget - used_budget < step) {
                            step = extra_budget - used_budget;
                        }
                        if (step == 0) break;

                        SerialLogger::Log("Hypervisor: Alpine boot still live; continuing for ");
                        SerialLogger::LogDec(step);
                        SerialLogger::Log(" more exits\r\n");

                        st = RunAlpineCycles(step);
                        used_budget += step;
                    }

                    // capture boot log from serial output
                    if (serial.HasOutput()) {
                        int n = serial.ReadOutput(alpine_boot_log,
                                                  (int)sizeof(alpine_boot_log) - 1);
                        if (n > 0) {
                            alpine_boot_log_len = n;
                            alpine_boot_log[n] = 0;
                        }
                    }

                    SerialLogger::Log("Hypervisor: Boot phase complete, state=");
                    SerialLogger::LogDec(st);
                    SerialLogger::Log(", captured ");
                    SerialLogger::LogDec(alpine_boot_log_len);
                    SerialLogger::Log(" bytes of boot log\r\n");

                    if (st == VM_STATE_RUNNING || st == VM_STATE_PAUSED) {
                        alpine_booted = true;
                        hw_ok = true;
                    }
                    if (alpine_boot_log_len > 0) {
                        alpine_booted = true;
                        hw_ok = true;
                    }

                    // under whpx, vm entry failure (crashed state) means
                    // nested virt truly isn't supported  -  fall back cleanly
                    if (st == VM_STATE_CRASHED && VMM::IsWHPX()) {
                        SerialLogger::Log("Hypervisor: WHPX VM entry failed  -  "
                                         "nested virt not supported by host\r\n");
                        hw_ok = false;
                    }
                }
            }
        }

        if (hw_ok) {
            // hardware boot succeeded  -  extract drivers and return
            int drv_count = ExtractAlpineDrivers();
            _auto_setup_alpine_userland();
            SerialLogger::Log("Hypervisor: Extracted ");
            SerialLogger::LogDec(drv_count);
            SerialLogger::Log(" drivers from Alpine (HW mode)\r\n");
            return true;
        }

        SerialLogger::Log("Hypervisor: HW Alpine boot failed  -  state=");
        SerialLogger::LogDec((int)vm_state);
        SerialLogger::Log(", exits=");
        SerialLogger::LogDec((int)stats.total_exits);
        SerialLogger::Log(", serial bytes=");
        SerialLogger::LogDec((int)stats.serial_bytes_tx);
        SerialLogger::Log("\r\n");

        // write a diagnostic boot log so the user sees why it failed
        if (alpine_boot_log_len == 0) {
            int di = 0;
            FillGenericVMEntryFailureDiag(alpine_boot_log, di,
                                          (int)sizeof(alpine_boot_log),
                                          "Alpine");
            alpine_boot_log[di] = 0;
            alpine_boot_log_len = di;
        }
        DestroyVM();
    } else {
        SerialLogger::Log("Hypervisor: No HW virt available");
        if (VMM::IsWHPX()) {
            SerialLogger::Log("  -  WHPX doesn't support nested virt\r\n");
            SerialLogger::Log("Hypervisor: QEMU's WHPX accelerator cannot emulate vmrun/vmlaunch.\r\n");
            SerialLogger::Log("Hypervisor: To get real HW Alpine, use KVM (Linux) or bare metal.\r\n");
        } else {
            SerialLogger::Log("\r\n");
        }
        SerialLogger::Log("Hypervisor: Alpine requires bare-metal AMD-V or VT-x. No fallback.\r\n");

        // write diagnostic into boot log
        if (alpine_boot_log_len == 0) {
            const char* diag =
                "[kurono] Alpine boot failed  -  no hardware virtualization.\n"
                "[kurono] VT-x/AMD-V was not detected on this system.\n"
                "[kurono] Enable Intel VT-x or AMD-V in BIOS settings,\n"
                "[kurono] or run Kurono on hardware that supports it.\n";
            int di = 0;
            while (diag[di] && di < (int)sizeof(alpine_boot_log) - 1) {
                alpine_boot_log[di] = diag[di];
                di++;
            }
            alpine_boot_log[di] = 0;
            alpine_boot_log_len = di;
        }
    }

    // no software fallback  -  honest failure beats fake success
    return false;
}

//  bootdebianwithextraction  -  boot embedded debian ext4 rootfs guest

bool Hypervisor::BootDebianWithExtraction(uint32_t max_boot_exits) {
    if (!alpine_kernel_data() || alpine_kernel_size() == 0) {
        SerialLogger::Log("Hypervisor: Shared Linux kernel not embedded\r\n");
        return false;
    }
    if (!debian_rootfs_available()) {
        SerialLogger::Log("Hypervisor: Debian rootfs not embedded\r\n");
        return false;
    }

    debian_boot_log_len = 0;
    debian_boot_log[0] = 0;
    debian_booted = false;

    SerialLogger::Log("Hypervisor: BootDebianWithExtraction starting\r\n");

    if (vm_state != VM_STATE_UNINITIALIZED &&
        vm_state != VM_STATE_DESTROYED) {
        DestroyVM();
    }
    Init();
    hw_available = VMM::IsSupported();

    if (!hw_available) {
        SerialLogger::Log("Hypervisor: Debian requires hardware virtualization\r\n");
        // provide diagnostic boot log
        const char* diag =
            "[kurono] Debian boot failed  -  no hardware virtualization.\n"
            "[kurono] VT-x/AMD-V was not detected on this system.\n"
            "[kurono] Enable Intel VT-x or AMD-V in BIOS, or run\n"
            "[kurono] Kurono on hardware that supports it.\n";
        int di = 0;
        while (diag[di] && di < (int)sizeof(debian_boot_log) - 1) {
            debian_boot_log[di] = diag[di];
            di++;
        }
        debian_boot_log[di] = 0;
        debian_boot_log_len = di;
        return false;
    }

    uint32_t rootfs_size = debian_rootfs_size();
    uint32_t rootfs_mb = (rootfs_size + (1024 * 1024 - 1)) / (1024 * 1024);
    uint32_t disk_mb = rootfs_mb + 64;
    if (disk_mb < 256) disk_mb = 256;

    VMConfig cfg;
    cfg.SetDefaults();
    cfg.ram_mb        = 256;
    cfg.disk_size_mb  = disk_mb;
    cfg.enable_serial = true;
    cfg.enable_disk   = true;
    cfg.enable_apic   = true;
    cfg.timer_tick_us = 10000;
    cfg.cmdline       = "console=ttyS0 earlyprintk=serial,ttyS0,115200 "
                        "root=/dev/sda rw rootfstype=ext4 init=/sbin/init "
                        "systemd.unit=multi-user.target nokaslr noapic "
                        "nosmp noacpi pci=off";

    if (!CreateVM(cfg)) {
        SerialLogger::Log("Hypervisor: Failed to create VM for Debian\r\n");
        return false;
    }

    if (!disk.LoadImage(debian_rootfs_data(), rootfs_size, 0)) {
        SerialLogger::Log("Hypervisor: Failed to load embedded Debian rootfs\r\n");
        DestroyVM();
        return false;
    }

    if (!LoadLinuxKernel(alpine_kernel_data(), alpine_kernel_size(), cfg.cmdline)) {
        SerialLogger::Log("Hypervisor: Failed to load shared Linux kernel for Debian\r\n");
        DestroyVM();
        return false;
    }

    VMState st = RunVM(max_boot_exits);
    uint32_t extra_budget = max_boot_exits;
    if (extra_budget < 250000) extra_budget = 250000;
    uint32_t used_budget = 0;
    while ((st == VM_STATE_RUNNING || st == VM_STATE_PAUSED) &&
           debian_boot_log_len == 0 &&
           used_budget < extra_budget) {
        uint32_t step = 25000;
        if (extra_budget - used_budget < step) step = extra_budget - used_budget;
        if (step == 0) break;
        st = RunDebianCycles(step);
        used_budget += step;
    }

    if (serial.HasOutput()) {
        int n = serial.ReadOutput(debian_boot_log, (int)sizeof(debian_boot_log) - 1);
        if (n > 0) {
            debian_boot_log_len = n;
            debian_boot_log[n] = 0;
        }
    }

    SerialLogger::Log("Hypervisor: Debian boot phase complete, state=");
    SerialLogger::LogDec(st);
    SerialLogger::Log(", captured ");
    SerialLogger::LogDec(debian_boot_log_len);
    SerialLogger::Log(" bytes of boot log\r\n");

    if (st == VM_STATE_RUNNING || st == VM_STATE_PAUSED || debian_boot_log_len > 0) {
        debian_booted = true;
        return true;
    }

    // write diagnostic into boot log so user sees why it failed
    if (debian_boot_log_len == 0) {
        int di = 0;
        FillGenericVMEntryFailureDiag(debian_boot_log, di,
                                      (int)sizeof(debian_boot_log),
                                      "Debian");
        debian_boot_log[di] = 0;
        debian_boot_log_len = di;
    }
    DestroyVM();
    return false;
}

// bootalpinesoftware  -  removed
// all stubs removed. alpine requires real hardware virtualization (amd-v / vt-x).
// honest failure > fake success.

// softwarealpineexec  -  removed
// no fake command responses. alpine runs real or not at all.

//  extractalpinedrivers  -  parse boot log + query /proc /sys for drivers
//
//  strategy:
//    1. parse the kernel boot log for "driver loaded" / module messages
//    2. if vm is still running, send commands via serial to enumerate:
//       - cat /proc/modules
//       - ls /sys/bus/pci/drivers
//       - cat /proc/devices
//       - cat /proc/cpuinfo
//    3. parse the output and register each driver into linuxdriverframework

// helpers for parsing serial output
static bool _starts_with(const char* str, const char* prefix) {
    while (*prefix) {
        if (*str != *prefix) return false;
        str++; prefix++;
    }
    return true;
}

static void _copy_until(char* dst, const char* src, char delim, int max) {
    int i = 0;
    while (src[i] && src[i] != delim && i < max - 1) {
        dst[i] = src[i]; i++;
    }
    dst[i] = 0;
}

static bool _contains_text(const char* str, const char* needle) {
    if (!str || !needle || !*needle) return false;
    for (int i = 0; str[i]; i++) {
        int j = 0;
        while (needle[j] && str[i + j] == needle[j]) j++;
        if (!needle[j]) return true;
    }
    return false;
}

static LinuxDriverCategory _categorize_module(const char* mod_name) {
    if (!mod_name || !*mod_name) return LDRV_CAT_OTHER;

    if (_starts_with(mod_name, "iwl") || _starts_with(mod_name, "ath") ||
        _starts_with(mod_name, "rtw") || _starts_with(mod_name, "rtl") ||
        _starts_with(mod_name, "brcm") || _starts_with(mod_name, "mt76") ||
        _starts_with(mod_name, "cfg80211") || _starts_with(mod_name, "mac80211")) {
        return LDRV_CAT_NET;
    }
    if (_starts_with(mod_name, "bluetooth") || _starts_with(mod_name, "bt") ||
        _contains_text(mod_name, "blue")) {
        return LDRV_CAT_OTHER;
    }
    if (_starts_with(mod_name, "nvidia") || _starts_with(mod_name, "amdgpu") ||
        _starts_with(mod_name, "radeon") || _starts_with(mod_name, "nouveau") ||
        _starts_with(mod_name, "i915") || _starts_with(mod_name, "xe") ||
        _starts_with(mod_name, "drm") || _starts_with(mod_name, "virtio_gpu") ||
        _starts_with(mod_name, "bochs")) {
        return LDRV_CAT_GPU;
    }
    if (_starts_with(mod_name, "snd") || _starts_with(mod_name, "hda") ||
        _starts_with(mod_name, "ac97")) {
        return LDRV_CAT_SOUND;
    }
    if (_starts_with(mod_name, "xhci") || _starts_with(mod_name, "ehci") ||
        _starts_with(mod_name, "uhci") || _starts_with(mod_name, "ohci") ||
        _starts_with(mod_name, "usb")) {
        return LDRV_CAT_BUS;
    }
    if (_starts_with(mod_name, "hid") || _starts_with(mod_name, "input") ||
        _starts_with(mod_name, "psmouse") || _starts_with(mod_name, "atkbd")) {
        return LDRV_CAT_INPUT;
    }
    if (_starts_with(mod_name, "ext4") || _starts_with(mod_name, "xfs") ||
        _starts_with(mod_name, "btrfs") || _starts_with(mod_name, "f2fs") ||
        _starts_with(mod_name, "overlay") || _starts_with(mod_name, "squashfs") ||
        _starts_with(mod_name, "tmpfs")) {
        return LDRV_CAT_FS;
    }
    if (_starts_with(mod_name, "virtio") || _starts_with(mod_name, "pci") ||
        _starts_with(mod_name, "acpi") || _starts_with(mod_name, "i2c") ||
        _starts_with(mod_name, "spi")) {
        return LDRV_CAT_BUS;
    }
    return LDRV_CAT_OTHER;
}

static void _register_metadata_binding(const char* drv_name, const char* description,
                                       LinuxDriverCategory category) {
    if (!drv_name || !*drv_name) return;
    LinuxDriver* existing = LinuxDriverFramework::FindDriver(drv_name);
    if (existing) {
        existing->category = category;
        existing->state = LDRV_ACTIVE;
        existing->bound = true;
        if (description && *description) {
            int i = 0;
            while (description[i] && i < LDRV_MAX_DESC - 1) {
                existing->description[i] = description[i];
                i++;
            }
            existing->description[i] = 0;
        }
        return;
    }

    LinuxDriver drv = {};
    int i = 0;
    while (drv_name[i] && i < LDRV_MAX_NAME - 1) { drv.name[i] = drv_name[i]; i++; }
    drv.name[i] = 0;
    i = 0;
    while (description && description[i] && i < LDRV_MAX_DESC - 1) { drv.description[i] = description[i]; i++; }
    drv.description[i] = 0;
    drv.version[0] = 'M'; drv.version[1] = 'E'; drv.version[2] = 'T'; drv.version[3] = 'A'; drv.version[4] = 0;
    drv.license[0] = 'G'; drv.license[1] = 'P'; drv.license[2] = 'L'; drv.license[3] = 0;
    drv.category = category;
    drv.state = LDRV_ACTIVE;
    drv.bound = true;
    LinuxDriverFramework::RegisterDriver(&drv);
}

static void _bind_native_radio_stack(const char* mod_name) {
    if (!mod_name || !*mod_name) return;

    if (_starts_with(mod_name, "iwl")) {
        _register_metadata_binding("wifi_intel", "Native Intel WiFi binding (Alpine metadata)", LDRV_CAT_NET);
    } else if (_starts_with(mod_name, "ath")) {
        _register_metadata_binding("wifi_atheros", "Native Atheros WiFi binding (Alpine metadata)", LDRV_CAT_NET);
    } else if (_starts_with(mod_name, "rtw") || _starts_with(mod_name, "rtl")) {
        _register_metadata_binding("wifi_realtek", "Native Realtek WiFi binding (Alpine metadata)", LDRV_CAT_NET);
    } else if (_starts_with(mod_name, "brcm")) {
        _register_metadata_binding("wifi_broadcom", "Native Broadcom WiFi binding (Alpine metadata)", LDRV_CAT_NET);
    } else if (_starts_with(mod_name, "mt76")) {
        _register_metadata_binding("wifi_mediatek", "Native MediaTek WiFi binding (Alpine metadata)", LDRV_CAT_NET);
    }

    if (_starts_with(mod_name, "cfg80211") || _starts_with(mod_name, "mac80211")) {
        _register_metadata_binding("wifi_stack", "Native 802.11 stack binding (Alpine metadata)", LDRV_CAT_NET);
    }

    if (_starts_with(mod_name, "bluetooth")) {
        _register_metadata_binding("bluetooth_core", "Native Bluetooth core binding (Alpine metadata)", LDRV_CAT_OTHER);
    } else if (_starts_with(mod_name, "btusb")) {
        _register_metadata_binding("bluetooth_usb", "Native USB Bluetooth binding (Alpine metadata)", LDRV_CAT_OTHER);
    } else if (_starts_with(mod_name, "btintel")) {
        _register_metadata_binding("bluetooth_intel", "Native Intel Bluetooth binding (Alpine metadata)", LDRV_CAT_OTHER);
    } else if (_starts_with(mod_name, "btrtl")) {
        _register_metadata_binding("bluetooth_realtek", "Native Realtek Bluetooth binding (Alpine metadata)", LDRV_CAT_OTHER);
    } else if (_starts_with(mod_name, "hci_uart")) {
        _register_metadata_binding("bluetooth_uart", "Native UART Bluetooth binding (Alpine metadata)", LDRV_CAT_OTHER);
    }
}

static void _auto_setup_alpine_userland() {
    if (!Hypervisor::IsAlpineBooted()) return;

    char result[2048];
    int n = 0;

    SerialLogger::Log("Hypervisor: Alpine auto-setup starting\r\n");

    Hypervisor::AlpineExec(
        "grep -q 'edge/community' /etc/apk/repositories || echo 'http://dl-cdn.alpinelinux.org/alpine/edge/community' >> /etc/apk/repositories",
        result, (int)sizeof(result) - 1);
    Hypervisor::AlpineExec(
        "grep -q 'edge/testing' /etc/apk/repositories || echo 'http://dl-cdn.alpinelinux.org/alpine/edge/testing' >> /etc/apk/repositories",
        result, (int)sizeof(result) - 1);
    Hypervisor::AlpineExec("apk update 2>/dev/null", result, (int)sizeof(result) - 1);

    n = Hypervisor::AlpineExec("which ffmpeg 2>/dev/null", result, (int)sizeof(result) - 1);
    if (n <= 0) {
        SerialLogger::Log("Hypervisor: Installing Alpine ffmpeg bridge\r\n");
        Hypervisor::AlpineExec("apk add --no-cache ffmpeg 2>&1", result, (int)sizeof(result) - 1);
    }

    n = Hypervisor::AlpineExec("which pwsh 2>/dev/null || which powershell 2>/dev/null",
                               result, (int)sizeof(result) - 1);
    if (n <= 0) {
        SerialLogger::Log("Hypervisor: Installing Alpine PowerShell bridge\r\n");
        Hypervisor::AlpineExec(
            "apk add --no-cache powershell 2>&1 || apk add --no-cache pwsh 2>&1",
            result, (int)sizeof(result) - 1);
    }

    n = Hypervisor::AlpineExec("pwsh --version 2>/dev/null", result, (int)sizeof(result) - 1);
    if (n > 0) {
        KuronoShell::pwsh_available = true;
        KuronoShell::alpine_cmd_cached = false;
        SerialLogger::Log("Hypervisor: Alpine PowerShell ready\r\n");
    }

    Hypervisor::AlpineExec("lspci -nnk 2>/dev/null | head -80", result, (int)sizeof(result) - 1);
    Hypervisor::AlpineExec("cat /proc/modules 2>/dev/null | head -80", result, (int)sizeof(result) - 1);
    Hypervisor::AlpineExec("rfkill list 2>/dev/null || true", result, (int)sizeof(result) - 1);
    Hypervisor::AlpineExec("hciconfig -a 2>/dev/null || true", result, (int)sizeof(result) - 1);

    SerialLogger::Log("Hypervisor: Alpine auto-setup complete\r\n");
}

int Hypervisor::ExtractAlpineDrivers() {
    int registered = 0;

    // linux kernel boot messages that indicate a driver loaded:
    //   "[    0.123456] e1000: intel(r) pro/1000 network driver"
    //   "[    0.123456] serial8250: ttys0 at i/o 0x3f8"
    //   "[    0.123456] input: at translated set 2 keyboard"
    //   "[    0.123456] scsi subsystem initialized"

    // known alpine kernel driver signatures to look for in boot log
    struct BootLogDriver {
        const char* search;       // string to find in boot log
        const char* drv_name;     // driver name to register
        const char* description;  // description
        LinuxDriverCategory cat;  // category
    };

    static const BootLogDriver boot_drivers[] = {
        { "serial8250",     "serial8250",   "Alpine 8250/16550 UART",               LDRV_CAT_CHAR },
        { "ttyS0",          "ttyS0",        "Alpine serial console",                LDRV_CAT_CHAR },
        { "e1000",          "e1000",        "Alpine Intel PRO/1000 Ethernet",       LDRV_CAT_NET },
        { "iwlwifi",        "iwlwifi",      "Alpine Intel WiFi",                    LDRV_CAT_NET },
        { "ath",            "ath_wifi",     "Alpine Atheros WiFi",                  LDRV_CAT_NET },
        { "rtw",            "rtw_wifi",     "Alpine Realtek WiFi",                  LDRV_CAT_NET },
        { "brcm",           "brcm_wifi",    "Alpine Broadcom WiFi",                 LDRV_CAT_NET },
        { "cfg80211",       "cfg80211",     "Alpine cfg80211 wireless stack",       LDRV_CAT_NET },
        { "mac80211",       "mac80211",     "Alpine mac80211 wireless stack",       LDRV_CAT_NET },
        { "bluetooth",      "bluetooth",    "Alpine Bluetooth core",                LDRV_CAT_OTHER },
        { "btusb",          "btusb",        "Alpine USB Bluetooth host",            LDRV_CAT_OTHER },
        { "btintel",        "btintel",      "Alpine Intel Bluetooth transport",     LDRV_CAT_OTHER },
        { "hci_uart",       "hci_uart",     "Alpine UART Bluetooth transport",      LDRV_CAT_OTHER },
        { "virtio",         "virtio_pci",   "Alpine VirtIO PCI transport",          LDRV_CAT_BUS },
        { "virtio_net",     "virtio_net",   "Alpine VirtIO network",                LDRV_CAT_NET },
        { "virtio_blk",     "virtio_blk",   "Alpine VirtIO block device",           LDRV_CAT_BLOCK },
        { "virtio_gpu",     "virtio_gpu",   "Alpine VirtIO GPU",                    LDRV_CAT_GPU },
        { "SCSI",           "scsi_mod",     "Alpine SCSI subsystem",                LDRV_CAT_BLOCK },
        { "sd ",            "sd_mod",       "Alpine SCSI disk",                     LDRV_CAT_BLOCK },
        { "i8042",          "i8042",        "Alpine i8042 PS/2 controller",         LDRV_CAT_BUS },
        { "atkbd",          "atkbd",        "Alpine AT keyboard",                   LDRV_CAT_INPUT },
        { "psmouse",        "psmouse",      "Alpine PS/2 mouse",                    LDRV_CAT_INPUT },
        { "input:",         "evdev",        "Alpine input event device",            LDRV_CAT_INPUT },
        { "bochs",          "bochs_drm",    "Alpine Bochs DRM framebuffer",         LDRV_CAT_GPU },
        { "fb0:",           "fbdev",        "Alpine framebuffer device",            LDRV_CAT_GPU },
        { "ehci",           "ehci_hcd",     "Alpine EHCI USB host",                 LDRV_CAT_BUS },
        { "xhci",           "xhci_hcd",     "Alpine xHCI USB host",                 LDRV_CAT_BUS },
        { "snd",            "snd_pcm",      "Alpine ALSA PCM sound",                LDRV_CAT_SOUND },
        { "ACPI",           "acpi",         "Alpine ACPI bus driver",               LDRV_CAT_POWER },
        { "PCI",            "pci_bus",      "Alpine PCI bus driver",                LDRV_CAT_BUS },
        { "RTC",            "rtc_cmos",     "Alpine RTC CMOS driver",               LDRV_CAT_CHAR },
        { "loop:",          "loop",         "Alpine loopback block device",          LDRV_CAT_BLOCK },
        { "ext4",           "ext4",         "Alpine ext4 filesystem",               LDRV_CAT_FS },
        { "tmpfs",          "tmpfs",        "Alpine tmpfs filesystem",              LDRV_CAT_FS },
        { "squashfs",       "squashfs",     "Alpine squashfs filesystem",           LDRV_CAT_FS },
        { "overlay",        "overlayfs",    "Alpine overlay filesystem",            LDRV_CAT_FS },
        { "NET:",           "net_core",     "Alpine networking core",               LDRV_CAT_NET },
        { "TCP",            "tcp_ipv4",     "Alpine TCP/IPv4 stack",                LDRV_CAT_NET },
        { "random:",        "random",       "Alpine random number generator",       LDRV_CAT_CHAR },
    };

    // scan boot log for each driver signature
    for (int d = 0; d < (int)(sizeof(boot_drivers) / sizeof(boot_drivers[0])); d++) {
        const char* needle = boot_drivers[d].search;
        int nlen = 0;
        while (needle[nlen]) nlen++;

        bool found = false;
        for (int i = 0; i <= alpine_boot_log_len - nlen; i++) {
            bool match = true;
            for (int j = 0; j < nlen; j++) {
                if (alpine_boot_log[i + j] != needle[j]) { match = false; break; }
            }
            if (match) { found = true; break; }
        }

        if (found) {
            // check if already registered (don't duplicate)
            if (LinuxDriverFramework::FindDriver(boot_drivers[d].drv_name)) continue;

            LinuxDriver drv = {};
            int ni = 0;
            const char* src = boot_drivers[d].drv_name;
            while (src[ni] && ni < LDRV_MAX_NAME - 1) { drv.name[ni] = src[ni]; ni++; }
            drv.name[ni] = 0;

            ni = 0; src = boot_drivers[d].description;
            while (src[ni] && ni < LDRV_MAX_DESC - 1) { drv.description[ni] = src[ni]; ni++; }
            drv.description[ni] = 0;

            drv.version[0] = 'A'; drv.version[1] = 'L'; drv.version[2] = 'P';
            drv.version[3] = 0; // "alp"  -  alpine-sourced
            drv.license[0] = 'G'; drv.license[1] = 'P'; drv.license[2] = 'L';
            drv.license[3] = 0;
            drv.category = boot_drivers[d].cat;
            drv.state = LDRV_ACTIVE;
            drv.bound = true;

            LinuxDriverFramework::RegisterDriver(&drv);
            _bind_native_radio_stack(boot_drivers[d].drv_name);
            registered++;
        }
    }

    if (vm_state == VM_STATE_RUNNING) {
        char proc_buf[2048];

        // query loaded kernel modules
        int n = AlpineExec("cat /proc/modules 2>/dev/null", proc_buf, (int)sizeof(proc_buf) - 1);
        if (n > 0) {
            proc_buf[n] = 0;

            // parse /proc/modules format: "module_name size refcount deps state addr"
            // extract each module name (first word on each line)
            int pos = 0;
            while (pos < n) {
                // skip whitespace
                while (pos < n && (proc_buf[pos] == ' ' || proc_buf[pos] == '\t')) pos++;
                if (pos >= n || proc_buf[pos] == '\n') { pos++; continue; }

                // extract module name
                char mod_name[LDRV_MAX_NAME];
                int mi = 0;
                while (pos < n && proc_buf[pos] != ' ' && proc_buf[pos] != '\n'
                       && mi < LDRV_MAX_NAME - 1) {
                    mod_name[mi++] = proc_buf[pos++];
                }
                mod_name[mi] = 0;

                // skip to end of line
                while (pos < n && proc_buf[pos] != '\n') pos++;
                if (pos < n) pos++;

                if (mi == 0) continue;
                // already registered?
                if (LinuxDriverFramework::FindDriver(mod_name)) continue;

                LinuxDriver drv = {};
                for (int i = 0; i < mi && i < LDRV_MAX_NAME - 1; i++) drv.name[i] = mod_name[i];
                drv.name[mi] = 0;

                // description: "alpine module: <name>"
                const char* prefix = "Alpine module: ";
                int pi = 0;
                while (prefix[pi] && pi < LDRV_MAX_DESC - mi - 1) { drv.description[pi] = prefix[pi]; pi++; }
                for (int i = 0; i < mi && pi < LDRV_MAX_DESC - 1; i++) drv.description[pi++] = mod_name[i];
                drv.description[pi] = 0;

                drv.version[0] = 'A'; drv.version[1] = 'L'; drv.version[2] = 'P';
                drv.version[3] = 0;
                drv.license[0] = 'G'; drv.license[1] = 'P'; drv.license[2] = 'L';
                drv.license[3] = 0;
                drv.category = _categorize_module(mod_name);
                drv.state = LDRV_ACTIVE;
                drv.bound = true;

                LinuxDriverFramework::RegisterDriver(&drv);
                _bind_native_radio_stack(mod_name);
                registered++;
            }
        }

        // query /proc/devices for char/block device numbers
        n = AlpineExec("cat /proc/devices 2>/dev/null", proc_buf, (int)sizeof(proc_buf) - 1);
        if (n > 0) {
            proc_buf[n] = 0;
            // parse: "character devices:\n  1 mem\n  4 tty\n..."
            // we just log this for now  -  the boot log parse above covers most
            SerialLogger::Log("Hypervisor: /proc/devices (");
            SerialLogger::LogDec(n);
            SerialLogger::Log(" bytes):\r\n");
            // append to boot log if space
            if (alpine_boot_log_len + n + 32 < (int)sizeof(alpine_boot_log)) {
                const char* sep = "\n--- /proc/devices ---\n";
                for (int i = 0; sep[i] && alpine_boot_log_len < (int)sizeof(alpine_boot_log) - 1; i++)
                    alpine_boot_log[alpine_boot_log_len++] = sep[i];
                for (int i = 0; i < n && alpine_boot_log_len < (int)sizeof(alpine_boot_log) - 1; i++)
                    alpine_boot_log[alpine_boot_log_len++] = proc_buf[i];
                alpine_boot_log[alpine_boot_log_len] = 0;
            }
        }

        // query /proc/cpuinfo to capture guest cpu
        n = AlpineExec("head -20 /proc/cpuinfo 2>/dev/null", proc_buf, (int)sizeof(proc_buf) - 1);
        if (n > 0) {
            proc_buf[n] = 0;
            if (alpine_boot_log_len + n + 32 < (int)sizeof(alpine_boot_log)) {
                const char* sep = "\n--- /proc/cpuinfo ---\n";
                for (int i = 0; sep[i] && alpine_boot_log_len < (int)sizeof(alpine_boot_log) - 1; i++)
                    alpine_boot_log[alpine_boot_log_len++] = sep[i];
                for (int i = 0; i < n && alpine_boot_log_len < (int)sizeof(alpine_boot_log) - 1; i++)
                    alpine_boot_log[alpine_boot_log_len++] = proc_buf[i];
                alpine_boot_log[alpine_boot_log_len] = 0;
            }
        }
    }

    return registered;
}

//  runalpinecycles  -  incremental alpine vm execution

VMState Hypervisor::RunAlpineCycles(uint32_t max_exits) {
    if (vm_state != VM_STATE_RUNNING && vm_state != VM_STATE_PAUSED) {
        return vm_state;
    }
    if (vm_state == VM_STATE_PAUSED) {
        vm_state = VM_STATE_RUNNING;
    }

    for (uint32_t i = 0; i < max_exits && vm_state == VM_STATE_RUNNING; i++) {
        vm_state = RunOneCycle();
        if (i % 100 == 0) TickDevices();
    }

    // capture any new serial output into boot log
    if (serial.HasOutput()) {
        int remain = (int)sizeof(alpine_boot_log) - alpine_boot_log_len - 1;
        if (remain > 0) {
            int n = serial.ReadOutput(alpine_boot_log + alpine_boot_log_len, remain);
            if (n > 0) {
                alpine_boot_log_len += n;
                alpine_boot_log[alpine_boot_log_len] = 0;
            }
        } else {
            // boot log full  -  just drain and discard
            char discard[256];
            serial.ReadOutput(discard, (int)sizeof(discard) - 1);
        }
    }

    return vm_state;
}

VMState Hypervisor::RunDebianCycles(uint32_t max_exits) {
    if (vm_state != VM_STATE_RUNNING && vm_state != VM_STATE_PAUSED) {
        return vm_state;
    }
    if (vm_state == VM_STATE_PAUSED) {
        vm_state = VM_STATE_RUNNING;
    }

    for (uint32_t i = 0; i < max_exits && vm_state == VM_STATE_RUNNING; i++) {
        vm_state = RunOneCycle();
        if (i % 100 == 0) TickDevices();
    }

    if (serial.HasOutput()) {
        int remain = (int)sizeof(debian_boot_log) - debian_boot_log_len - 1;
        if (remain > 0) {
            int n = serial.ReadOutput(debian_boot_log + debian_boot_log_len, remain);
            if (n > 0) {
                debian_boot_log_len += n;
                debian_boot_log[debian_boot_log_len] = 0;
            }
        } else {
            char discard[256];
            serial.ReadOutput(discard, (int)sizeof(discard) - 1);
        }
    }

    return vm_state;
}

//  alpineexec  -  send command to guest, run cycles, capture output

int Hypervisor::AlpineExec(const char* cmd, char* out_buf, int out_max) {
    if (!cmd || out_max <= 0) return 0;

    if (vm_state != VM_STATE_RUNNING) return 0;

    // drain any pending serial output first so we don't mix old data
    {
        char drain[512];
        while (serial.HasOutput()) serial.ReadOutput(drain, (int)sizeof(drain) - 1);
    }

    // send the command + newline
    serial.InjectRxString(cmd);
    serial.InjectRxByte('\n');
    for (const char* p = cmd; *p; p++) stats.serial_bytes_rx++;
    stats.serial_bytes_rx++;

    // run vm cycles to let the guest process the command
    // we use a reasonable number of exits to allow the guest to execute
    int total_read = 0;
    for (int attempt = 0; attempt < 10; attempt++) {
        // run 5000 exits per attempt
        for (uint32_t i = 0; i < 5000 && vm_state == VM_STATE_RUNNING; i++) {
            vm_state = RunOneCycle();
            if (i % 100 == 0) TickDevices();
        }

        // read whatever the guest produced
        if (serial.HasOutput()) {
            int n = serial.ReadOutput(out_buf + total_read, out_max - total_read - 1);
            if (n > 0) total_read += n;
        }

        // if we got some output and guest likely finished (no more coming for a cycle)
        if (total_read > 0 && !serial.HasOutput()) {
            // run a few more cycles to see if more output comes
            for (uint32_t i = 0; i < 1000 && vm_state == VM_STATE_RUNNING; i++) {
                vm_state = RunOneCycle();
            }
            if (!serial.HasOutput()) break; // cmd finished
            // still output coming  -  read more
            int n = serial.ReadOutput(out_buf + total_read, out_max - total_read - 1);
            if (n > 0) total_read += n;
        }

        if (vm_state != VM_STATE_RUNNING) break;
    }

    if (total_read > 0) out_buf[total_read] = 0;
    return total_read;
}

int Hypervisor::DebianExec(const char* cmd, char* out_buf, int out_max) {
    if (!cmd || out_max <= 0) return 0;
    if (vm_state != VM_STATE_RUNNING) return 0;

    {
        char drain[512];
        while (serial.HasOutput()) serial.ReadOutput(drain, (int)sizeof(drain) - 1);
    }

    serial.InjectRxString(cmd);
    serial.InjectRxByte('\n');
    for (const char* p = cmd; *p; p++) stats.serial_bytes_rx++;
    stats.serial_bytes_rx++;

    int total_read = 0;
    for (int attempt = 0; attempt < 10; attempt++) {
        for (uint32_t i = 0; i < 5000 && vm_state == VM_STATE_RUNNING; i++) {
            vm_state = RunOneCycle();
            if (i % 100 == 0) TickDevices();
        }

        if (serial.HasOutput()) {
            int n = serial.ReadOutput(out_buf + total_read, out_max - total_read - 1);
            if (n > 0) total_read += n;
        }

        if (total_read > 0 && !serial.HasOutput()) {
            for (uint32_t i = 0; i < 1000 && vm_state == VM_STATE_RUNNING; i++) {
                vm_state = RunOneCycle();
            }
            if (!serial.HasOutput()) break;
            int n = serial.ReadOutput(out_buf + total_read, out_max - total_read - 1);
            if (n > 0) total_read += n;
        }

        if (vm_state != VM_STATE_RUNNING) break;
    }

    if (total_read > 0) out_buf[total_read] = 0;
    return total_read;
}

//  alpine state accessors

bool Hypervisor::IsAlpineBooted() { return alpine_booted; }
const char* Hypervisor::GetAlpineBootLog() { return alpine_boot_log; }
int Hypervisor::GetAlpineBootLogLen() { return alpine_boot_log_len; }
bool Hypervisor::IsDebianBooted() { return debian_booted; }
const char* Hypervisor::GetDebianBootLog() { return debian_boot_log; }
int Hypervisor::GetDebianBootLogLen() { return debian_boot_log_len; }

//  statistics

void Hypervisor::ResetStats() {
    memset(&stats, 0, sizeof(stats));
}

//  debug

void Hypervisor::DumpState() {
    SerialLogger::Log("=== Hypervisor State ===\r\n");
    SerialLogger::Log("  State: ");
    switch (vm_state) {
        case VM_STATE_UNINITIALIZED: SerialLogger::Log("UNINITIALIZED"); break;
        case VM_STATE_CREATED:       SerialLogger::Log("CREATED");       break;
        case VM_STATE_RUNNING:       SerialLogger::Log("RUNNING");       break;
        case VM_STATE_PAUSED:        SerialLogger::Log("PAUSED");        break;
        case VM_STATE_HALTED:        SerialLogger::Log("HALTED");        break;
        case VM_STATE_CRASHED:       SerialLogger::Log("CRASHED");       break;
        case VM_STATE_REBOOTING:     SerialLogger::Log("REBOOTING");     break;
        case VM_STATE_DESTROYED:     SerialLogger::Log("DESTROYED");     break;
    }
    SerialLogger::Log("\r\n");

    SerialLogger::Log("  HW available: ");
    SerialLogger::Log(hw_available ? "yes" : "no");
    SerialLogger::Log("\r\n");

    SerialLogger::Log("  RAM: ");
    SerialLogger::LogDec(config.ram_mb);
    SerialLogger::Log(" MB\r\n");

    SerialLogger::Log("  Total exits: ");
    SerialLogger::LogDec(stats.total_exits);
    SerialLogger::Log("  I/O: ");
    SerialLogger::LogDec(stats.io_exits);
    SerialLogger::Log("  MMIO: ");
    SerialLogger::LogDec(stats.mmio_exits);
    SerialLogger::Log("\r\n");

    SerialLogger::Log("  IRQ injections: ");
    SerialLogger::LogDec(stats.irq_injections);
    SerialLogger::Log("  Run cycles: ");
    SerialLogger::LogDec(stats.run_cycles);
    SerialLogger::Log("\r\n");

    SerialLogger::Log("  Serial TX: ");
    SerialLogger::LogDec(stats.serial_bytes_tx);
    SerialLogger::Log("  RX: ");
    SerialLogger::LogDec(stats.serial_bytes_rx);
    SerialLogger::Log("\r\n");

    if (vcpu) {
        VMM::DumpVCPUState(vcpu);
    }

    VMExitHandler::DumpStats();
    GuestMemoryManager::DumpMemoryMap();
    GuestMemoryManager::DumpE820();
}

void Hypervisor::DumpGuestRegs() {
    if (!vcpu) return;

    SerialLogger::Log("=== Guest Registers ===\r\n");
    const char* names[] = {"EAX","ECX","EDX","EBX","ESP","EBP","ESI","EDI"};
    for (int i = 0; i < 8; i++) {
        SerialLogger::Log("  ");
        SerialLogger::Log(names[i]);
        SerialLogger::Log("=");
        SerialLogger::LogHex(vcpu->regs[i]);
        if (i % 4 == 3) SerialLogger::Log("\r\n");
        else SerialLogger::Log("  ");
    }

    if (vcpu->type == VIRT_INTEL_VTX && vcpu->vmcs) {
        VMM::VMPtrLoad(vcpu->vmcs);
        SerialLogger::Log("  EIP=");
        SerialLogger::LogHex(VMM::VMRead(VMCS_GUEST_RIP));
        SerialLogger::Log("  EFLAGS=");
        SerialLogger::LogHex(VMM::VMRead(VMCS_GUEST_RFLAGS));
        SerialLogger::Log("\r\n");
        SerialLogger::Log("  CR0=");
        SerialLogger::LogHex(VMM::VMRead(VMCS_GUEST_CR0));
        SerialLogger::Log("  CR3=");
        SerialLogger::LogHex(VMM::VMRead(VMCS_GUEST_CR3));
        SerialLogger::Log("  CR4=");
        SerialLogger::LogHex(VMM::VMRead(VMCS_GUEST_CR4));
        SerialLogger::Log("\r\n");
    } else if (vcpu->type == VIRT_AMD_SVM && vcpu->vmcb) {
        SerialLogger::Log("  RIP=");
        SerialLogger::LogHex((uint32_t)vcpu->vmcb->rip);
        SerialLogger::Log("  RFLAGS=");
        SerialLogger::LogHex((uint32_t)vcpu->vmcb->rflags);
        SerialLogger::Log("\r\n");
    }
}
