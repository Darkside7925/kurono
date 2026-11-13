//  kurono os - hypervisor / vm lifecycle manager implementation
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
#include "../linux/linux_syscall.h"   // kls released while the guest runs (satoru)
#include "hypervisor.h"
#include "mb2_boot.h"   // multiboot2 guests (satoru)
#include "vi8042.h"     // their ps/2 controller (satoru)
#include "../kernel/kimage.h"   // heap direct-map pointers -> phys (satoru)
#include "../system/kasset.h"
#include "../kernel/types.h"
#include "../kernel/heap.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include "../linux/linux_drivers.h"
#include "../shell/shell.h"
#include "vpci.h"
#include "virtio_gpu_host.h"
#include "virtio_dev.h"
#include "vblk.h"
#include "vballoon.h"
#include "vlapic.h"
#include "../proc/smp.h"
#include "../drivers/cpu_detect.h"
#include "vcon.h"
#include "vnet.h"
#include "vm_host.h"
#include "../proc/scheduler.h"
#include "../drivers/disklog.h"
#include "pci_passthrough.h"

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

// rolling tail of guest serial text, see NoteGuestText (satoru)
static char g_guest_tail[192];
static int  g_guest_tail_len = 0;
static bool g_guest_prompt_seen = false;
static bool g_guest_panic_seen = false;

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
bool         Hypervisor::guest_halted      = false;
vCPU*        Hypervisor::vcpus[8]          = {};
int          Hypervisor::nvcpus            = 1;
int          Hypervisor::cur_vcpu          = 0;
int          Hypervisor::want_cpus         = 1;
uint8_t      Hypervisor::ap_state[8]       = {};
bool         Hypervisor::vhalted[8]        = {};
bool         Hypervisor::yield_req         = false;
bool         Hypervisor::reboot_requested  = false;
void*        Hypervisor::owner_proc        = nullptr;
bool         Hypervisor::alpine_booted     = false;
char         Hypervisor::alpine_boot_log[8192];
int          Hypervisor::alpine_boot_log_len = 0;
bool         Hypervisor::debian_booted     = false;
char         Hypervisor::debian_boot_log[8192];
int          Hypervisor::debian_boot_log_len = 0;
bool         Hypervisor::linux_guest_enabled = true;
bool         Hypervisor::mb2_mode = false;   // (satoru)
static bool  mb2_guest = false;              // a multiboot2 (kurono) guest is up (satoru)
bool Hypervisor::Mb2Guest() { return mb2_guest; }
bool         Hypervisor::i8042_on = false;   // (satoru)
uint32_t     Hypervisor::mb2_entry = 0;
uint32_t     Hypervisor::mb2_info = 0;
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

//  init - detect hardware virtualization

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
            SerialLogger::Log(" - WHPX detected but nested virt blocked by host");
        }
        SerialLogger::Log(" - software emulation only\r\n");
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

//  createvm - set up a new virtual machine

bool Hypervisor::CreateVM(const VMConfig& cfg) {
    if (ForeignCaller()) return false;   // (satoru)

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

    VMM::ResetGuestFPU();               // (satoru)
    VMExitHandler::ResetGuestClock();
    VMExitHandler::ResetLogBudget();
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

    nvcpus = 1; cur_vcpu = 0;
    for (int i = 0; i < 8; i++) { vcpus[i] = nullptr; ap_state[i] = 0; vhalted[i] = false; }
    vcpus[0] = vcpu;
    ap_state[0] = 1;
    if (VMHost::Par() && want_cpus > 1) {      // parallel vcpus need planned service cpus; else fewer (satoru)
        int ok = 1;
        while (ok < want_cpus && Scheduler::VcpuCpu(ok)) ok++;
        if (ok < want_cpus) want_cpus = ok;
    }
    if (hw_available && vcpu && vcpu->type == VIRT_INTEL_VTX && want_cpus > 1) {
        for (int k = 1; k < want_cpus && k < 8; k++) {
            vcpus[k] = VMM::CreateVCPU();
            if (!vcpus[k]) break;
            nvcpus = k + 1;
        }
        SerialLogger::Log("Hypervisor: smp guest, "); SerialLogger::LogDec(nvcpus); SerialLogger::Log(" vcpus\r\n");
    }
    VLapic::Reset(nvcpus, LapicIpi);
    if (nvcpus > 1) BuildMpTable(nvcpus);
    ParStart();

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

//  destroyvm - tear down and free all vm resources

// the background vm host's stop request, used when another thread wants the
// vm gone while the host owns it (satoru)
void (*g_hv_foreign_destroy)() = nullptr;

void Hypervisor::DestroyVM() {
    if (ForeignCaller()) {
        SerialLogger::Log("Hypervisor: DestroyVM from outside the vm host, asking it to stop\r\n");
        if (g_hv_foreign_destroy) g_hv_foreign_destroy();
        return;
    }
    SerialLogger::Log("Hypervisor: Destroying VM...\r\n");
    i8042_on = false;   // (satoru)
    mb2_guest = false;
    VNet::Shutdown();               // (satoru)
    DiskLog::DisarmGuestImage();

    ParStop();      // the ap threads clear their vmcs on their own cpus first (satoru)
    for (int k = 1; k < nvcpus; k++) if (vcpus[k]) { VMM::DestroyVCPU(vcpus[k]); vcpus[k] = nullptr; }
    nvcpus = 1; cur_vcpu = 0;
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

    PCIPassthrough::DetachGuest();   // bus master off before the frames go (satoru)
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

//  setupiobitmap - configure which i/o ports cause vm-exits

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
        VMM::VMWrite(VMCS_IO_BITMAP_A, KernelPhysOf(io_bitmap_a));
        VMM::VMWrite(VMCS_IO_BITMAP_B, KernelPhysOf(io_bitmap_b));
    }

    // amd svm: allocate a contiguous 12 kb iopm and set vmcb field
    // svm requires a single 12 kb region (3 pages) for the i/o permission map
    if (VMM::GetType() == VIRT_AMD_SVM && vcpu && vcpu->vmcb) {
        uint8_t* svm_iopm = (uint8_t*)HVAllocAligned(12288, 4096);
        if (svm_iopm) {
            memset(svm_iopm, 0xFF, 12288);  // intercept all i/o ports
            vcpu->vmcb->iopm_base_pa = KernelPhysOf(svm_iopm);
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

//  setupmsrbitmap - configure which msrs cause vm-exits

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
    // ia32_time_stamp_counter (0x10) - let guest read tsc directly
    uint32_t tsc_byte = 0x10 / 8;
    uint32_t tsc_bit  = 0x10 % 8;
    msr_bitmap[tsc_byte] &= ~(1 << tsc_bit);               // read pass-through
    // writes still exit: without tsc offsetting a guest wrmsr would move the
    // host's own tsc, which is the host scheduler clock (satoru)

    // write msr bitmap address to vmcs
    if (VMM::GetType() == VIRT_INTEL_VTX && vcpu && vcpu->vmcs) {
        VMM::VMPtrLoad(vcpu->vmcs);
        VMM::VMWrite(VMCS_MSR_BITMAP_ADDR, KernelPhysOf(msr_bitmap));
    }

    // amd svm: allocate an 8 kb msrpm and set vmcb field
    // svm requires 8 kb (2 pages) contiguous for the msr permission map
    if (VMM::GetType() == VIRT_AMD_SVM && vcpu && vcpu->vmcb) {
        uint8_t* svm_msrpm = (uint8_t*)HVAllocAligned(8192, 4096);
        if (svm_msrpm) {
            memset(svm_msrpm, 0xFF, 8192);  // intercept all msrs
            vcpu->vmcb->msrpm_base_pa = KernelPhysOf(svm_msrpm);
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

static bool g_par_on = false;                 // this vm runs parallel vcpus (satoru)
static inline uint64_t rdtsc_raw_h() { uint32_t lo, hi; asm volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((uint64_t)hi << 32) | lo; }
static volatile uint8_t g_ap_start[8];        // 0 parked, 1 sipi seen (start me), 2 running
static uint8_t g_ap_vec[8];
static volatile uint32_t g_ept_gen = 0;       // bumped when ept leaves are removed: other cpus invept
static volatile uint32_t g_seen_gen[8];       // the gen each vcpu thread flushed its ept tlb for
static volatile uint8_t  g_in_guest[8];       // vcpu k is inside the guest right now
static uint32_t g_lazy_mapped[(GUEST_MAX_HI_CHUNKS + 31) / 32];   // chunks with ept leaves (satoru)

//  setupept - create extended page tables mapping guest physical memory

bool Hypervisor::SetupEPT() {
    for (uint32_t i = 0; i < sizeof(g_lazy_mapped) / sizeof(g_lazy_mapped[0]); i++) g_lazy_mapped[i] = 0;
    if (VMM::GetType() == VIRT_INTEL_VTX) {
        ept_root = EPTManager::CreateEPT();
        if (!ept_root) return false;

        // map low ram (0 - 640 kb) = identity map to host allocation
        uint8_t* low_ram = GuestMemoryManager::GetLowRAM();
        uint32_t low_size = GuestMemoryManager::GetLowRAMSize();
        if (low_ram && low_size > 0) {
            EPTManager::MapRAM(ept_root, 0x00000000ULL,
                               KernelPhysOf(low_ram), low_size);
            EPTManager::AddRegion({0x00000000ULL,
                                   (uint64_t)(uintptr_t)low_ram,
                                   low_size, MEM_RAM, true, true, true});
        }

        // map vga buffer (0xa0000 - 0xbffff)
        uint8_t* vga = GuestMemoryManager::GetVGABuffer();
        if (vga) {
            EPTManager::MapMMIO(ept_root, GUEST_VGA_BASE,
                                KernelPhysOf(vga), (uint32_t)GUEST_VGA_SIZE);
        }

        // map rom area (0xc0000 - 0xfffff)
        uint8_t* rom = GuestMemoryManager::GetROMArea();
        if (rom) {
            EPTManager::MapROM(ept_root, GUEST_ROM_BASE,
                               KernelPhysOf(rom), (uint32_t)GUEST_ROM_SIZE);
        }

        // map high ram (1 mb+), one ept range per host-contiguous chunk (satoru)
        for (uint32_t ci = 0; ci < GuestMemoryManager::GetHighChunkCount(); ci++) {
            uint64_t cgpa = 0; uint8_t* chost = nullptr; uint32_t csz = 0;
            if (!GuestMemoryManager::GetHighChunk(ci, &cgpa, &chost, &csz)) break;
            if (!chost) continue;      // lazy ram: mapped on first touch (satoru)
            EPTManager::MapRAM(ept_root, cgpa, KernelPhysOf(chost), csz);
            g_lazy_mapped[ci / 32] |= 1u << (ci % 32);
        }
        EPTManager::AddRegion({GUEST_HIGH_RAM_START, 0, GuestMemoryManager::GetHighRAMSize(),
                               MEM_RAM, true, true, true});

        // map lapic mmio (0xfee00000)
        EPTManager::AddRegion({GUEST_LAPIC_BASE, 0, 0x1000, MEM_MMIO,
                               true, true, false});

        // map hpet mmio (0xfed00000)
        EPTManager::AddRegion({GUEST_HPET_BASE, 0, 0x400, MEM_MMIO,
                               true, true, false});

        // map vpci mmio window - virtio device bars live here
        EPTManager::AddRegion({VPCI_MMIO_BASE, 0, (uint32_t)VPCI_MMIO_SIZE,
                               MEM_MMIO, true, true, false});

        // write eptp to vmcs - single 64-bit write, do not split into
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

        for (uint32_t ci = 0; ci < GuestMemoryManager::GetHighChunkCount(); ci++) {   // (satoru)
            uint64_t cgpa = 0; uint8_t* chost = nullptr; uint32_t csz = 0;
            if (!GuestMemoryManager::GetHighChunk(ci, &cgpa, &chost, &csz)) break;
            if (!chost) continue;      // lazy ram (satoru)
            EPTManager::MapGuestPhysicalNPT(npt_root, cgpa, KernelPhysOf(chost), csz,
                NPT_PRESENT | NPT_WRITE | NPT_USER);
            g_lazy_mapped[ci / 32] |= 1u << (ci % 32);
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

//  setupdevices - initialize all virtual devices

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

    // virtual pci bus + virtio-gpu host device
    VPCI::Init();
    VirtioResetAll();
    VirtIOGPUHost::ForgetRegistration();
    if (config.enable_gpu && !VirtIOGPUHost::Init()) {
        SerialLogger::Log("Hypervisor: VirtIOGPUHost init failed (non-fatal)\r\n");
    }
    PCIPassthrough::Init();

    return true;
}

//  loadlinuxkernel - load a bzimage into guest memory

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

//  configureguestprotectedmode - set up vmcs/vmcb for 32-bit protected mode
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
        // host owns the cr0 fixed-1 bits (ne) so a guest write that clears
        // them traps instead of #gp'ing in non-root mode (sdm 25.3) (satoru)
        {
            uint64_t f0 = 0;
            VMM::SafeReadMSR(MSR_IA32_VMX_CR0_FIXED0, f0);
            VMM::VMWrite(VMCS_CR0_GUEST_HOST_MASK, f0 & ~((1ULL << 0) | (1ULL << 31)));
        }
        VMM::VMWrite(VMCS_GUEST_CR3, 0);
        // sdm 26.3.1.1: guest cr4 must obey ia32_vmx_cr4_fixed0, which has
        // vmxe set. cr4=0 is an invalid guest state. keep the fixed bits in
        // the real cr4, owned by the host, and show the guest 0 (satoru)
        {
            uint64_t f0 = 0, f1 = ~0ULL;
            VMM::SafeReadMSR(MSR_IA32_VMX_CR4_FIXED0, f0);
            VMM::SafeReadMSR(MSR_IA32_VMX_CR4_FIXED1, f1);
            VMM::VMWrite(VMCS_GUEST_CR4, f0 & f1);
            VMM::VMWrite(VMCS_CR4_GUEST_HOST_MASK, f0 | ~f1);
            VMM::VMWrite(VMCS_CR4_READ_SHADOW, 0);
        }
        VMM::VMWrite(VMCS_GUEST_IA32_EFER, 0); // (satoru)

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
        // have direct gpr fields - they're managed via the vm-exit save area.
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

//  runvm - main execution loop

VMState Hypervisor::RunVM(uint32_t max_exits) {
    if (ForeignCaller()) return vm_state;   // (satoru)

    if (vm_state != VM_STATE_CREATED) {
        SerialLogger::Log("Hypervisor: Cannot run - VM not ready\r\n");
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
        // createvm wrote the eptp and the bitmap addresses before vmxon and
        // vmptrld, so vmwrite dropped them (no current vmcs). the eptp stayed 0,
        // whose page-walk length field (bits 5:3) is not 3, which sdm 26.2.1.1
        // rejects with error 7 "invalid control field". write them into the
        // now-current vmcs (satoru)
        if (vcpu->type == VIRT_INTEL_VTX) {
            if (ept_root) VMM::VMWrite(VMCS_EPT_POINTER, EPTManager::BuildEPTP(ept_root));
            if (io_bitmap_a) VMM::VMWrite(VMCS_IO_BITMAP_A, KernelPhysOf(io_bitmap_a));
            if (io_bitmap_b) VMM::VMWrite(VMCS_IO_BITMAP_B, KernelPhysOf(io_bitmap_b));
            if (msr_bitmap) VMM::VMWrite(VMCS_MSR_BITMAP_ADDR, KernelPhysOf(msr_bitmap));
            for (uint32_t m = 0xC0000081; m <= 0xC0000084; m++) *VMM::GuestSwitchedMSR(m) = 0;
            *VMM::GuestSwitchedMSR(0xC0000102) = 0;
        }
    }

    SetupAPs();

    // now configure guest state for protected-mode linux entry
    // this must come after setupvcpu because setupvcpu sets real-mode defaults
    if (mb2_mode) {
        // multiboot2 entry, as GRUB does it: same flat 32-bit state, eax = the
        // magic, ebx = the info (regs[] is modrm order: 0 rax, 3 rbx) (satoru)
        ConfigureGuestProtectedMode(mb2_entry, 0);
        if (vcpu) { vcpu->regs[0] = 0x36d76289u; vcpu->regs[3] = mb2_info; }
    } else {
    uint32_t entry = LinuxBootLoader::GetEntryPoint();
    if (entry == 0) entry = LINUX_KERNEL_ADDR;
    ConfigureGuestProtectedMode(entry, LINUX_BOOT_PARAMS_ADDR);
    }

    vm_state = VM_STATE_RUNNING;
    SerialLogger::Log("Hypervisor: Entering VM run loop\r\n");

    // read tsc at start for time-based safety timeout
    uint32_t tsc_lo_start, tsc_hi_start;
    asm volatile("rdtsc" : "=a"(tsc_lo_start), "=d"(tsc_hi_start));
    uint64_t tsc_start = ((uint64_t)tsc_hi_start << 32) | tsc_lo_start;
    // ~3 seconds at 3 ghz - prevents the loop from blocking boot display
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

//  runonecycle - single vm-entry → vm-exit → handle cycle

VMState Hypervisor::RunOneCycle() {
    if (ForeignCaller()) return vm_state;   // (satoru)

    if (!vcpu) return VM_STATE_CRASHED;

    stats.run_cycles++;

    // check for pending interrupts to inject before re-entry
    CheckAndInjectPendingIRQs();

    if (!hw_available) {
        // no hardware virt - can't actually run guest
        // in production this would be a software interpreter
        if (stats.run_cycles <= 1) {
            SerialLogger::Log("Hypervisor: No HW virt, cannot execute guest\r\n");
        }
        return VM_STATE_HALTED;
    }

    // enter guest. the guest runs without kls; the exit is handled under it
    // again (the vm host loop holds it, see HostKls in vm_host.cpp). (satoru)
    int kls_depth = Scheduler::SchedV2() ? LinuxSyscall::KlsDropAll() : 0;
    int exit_reason = VMM::RunVCPU(vcpu);
    LinuxSyscall::KlsRestore(kls_depth);
    if (exit_reason < 0) {
        // vm-entry failed - under whpx this can happen if nested virt
        // isn't truly supported despite cpuid detection succeeding
        if (stats.run_cycles <= 1) {
            SerialLogger::Log("Hypervisor: VM-entry failed (exit_reason=");
            SerialLogger::LogDec(exit_reason);
            SerialLogger::Log(")\r\n");
            if (VMM::IsWHPX()) {
                SerialLogger::Log("Hypervisor: WHPX nested VM-entry rejected - "
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

// lazy guest ram: the guest touched a chunk with no ept mapping yet. back it with host
// frames (it may already be, a device model can touch it first) and map the whole chunk
// 4 kb leaf by leaf. (satoru)
bool Hypervisor::LazyMapRAM(uint64_t gpa) {
    if (!GuestMemoryManager::IsLazy()) return false;
    uint64_t cgpa = 0; uint8_t* chost = nullptr; uint32_t csz = 0;
    if (!GuestMemoryManager::EnsureHighChunk(gpa, &cgpa, &chost, &csz)) return false;
    uint32_t ci = GuestMemoryManager::ChunkIndex(cgpa);
    bool split = GuestMemoryManager::IsSplit(ci);
    if (!chost && !split) return false;
    uint32_t* w = &g_lazy_mapped[ci / 32];
    uint32_t bit = 1u << (ci % 32);
    if (*w & bit) {
        // mapped already. with parallel vcpus another cpu may have just done it: the retry then succeeds;
        // otherwise this violation is something else (satoru)
        if (VMM::GetType() != VIRT_AMD_SVM && ept_root) { uint64_t* lf = EPTManager::KmemxLeafEntry(ept_root, gpa & ~0xFFFULL); return lf && (*lf & 7); }
        return false;
    }
    if (VMM::GetType() == VIRT_AMD_SVM) {
        if (!npt_root) return false;
        if (split) {
            for (uint32_t k = 0; k * 4096 < csz; k++) {
                uint8_t* pg = nullptr; GuestMemoryManager::SplitPage(ci, k, &pg);
                EPTManager::MapGuestPhysicalNPT(npt_root, cgpa + k * 4096, KernelPhysOf(pg), 4096, NPT_PRESENT | NPT_WRITE | NPT_USER);
            }
        } else EPTManager::MapGuestPhysicalNPT(npt_root, cgpa, KernelPhysOf(chost), csz, NPT_PRESENT | NPT_WRITE | NPT_USER);
    } else {
        if (!ept_root) return false;
        if (split) {
            for (uint32_t k = 0; k * 4096 < csz; k++) {
                uint8_t* pg = nullptr; GuestMemoryManager::SplitPage(ci, k, &pg);
                EPTManager::MapRAM(ept_root, cgpa + k * 4096, KernelPhysOf(pg), 4096);
            }
        } else EPTManager::MapRAM(ept_root, cgpa, KernelPhysOf(chost), csz);
    }
    *w |= bit;
    return true;
}

uint64_t Hypervisor::ReleaseGuestRam(uint64_t gpa, uint64_t len) {
    if (!GuestMemoryManager::IsLazy() || !ept_root || VMM::GetType() != VIRT_INTEL_VTX) return 0;
    uint64_t freed = 0;
    uint64_t end = gpa + len;
    if (end < gpa) return 0;
    for (uint32_t ci = GuestMemoryManager::ChunkIndex(gpa); ; ci++) {
        uint64_t b = 0; uint8_t* h = nullptr; uint32_t sz = 0;
        if (!GuestMemoryManager::GetHighChunk(ci, &b, &h, &sz)) break;
        if (b >= end) break;
        if (b < gpa || b + sz > end || !GuestMemoryManager::HighChunkBacked(ci)) continue;       // partly covered or not backed
        EPTManager::UnmapRange(ept_root, b, sz);
        EPTManager::InvalidateEPT();
        if (g_par_on) {
            // another vcpu may hold a cached translation of the leaves just cleared. the host frames go
            // back to the pool below, so first make every other vcpu leave the guest and flush: it cannot
            // re-enter before it saw the new generation (satoru)
            uint32_t gen = __atomic_add_fetch(&g_ept_gen, 1u, __ATOMIC_SEQ_CST);
            g_seen_gen[cur_vcpu] = gen;
            uint64_t guard = rdtsc_raw_h() + 4000000000ULL;
            for (;;) {
                bool wait = false;
                for (int k = 0; k < nvcpus; k++) {
                    if (k == cur_vcpu) continue;
                    if (g_in_guest[k] && g_seen_gen[k] != gen) { wait = true; KickVcpu(k); }
                }
                if (!wait || rdtsc_raw_h() > guard) break;
                __asm__ volatile("pause");
            }
        }
        uint64_t rb = 0; uint32_t rs = 0;
        if (GuestMemoryManager::ReleaseHighChunk(ci, &rb, &rs)) {
            g_lazy_mapped[ci / 32] &= ~(1u << (ci % 32));
            freed += rs;
        }
    }
    return freed;
}

//  processvmexit - dispatch vm-exit to handlers

static uint32_t HVApicId() {
    uint32_t a, b, c, d;
    asm volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    return b >> 24;
}

uint32_t g_hv_reason_hist[80];

VMState Hypervisor::ProcessVMExit() {
    if (!vcpu) return VM_STATE_CRASHED;

    stats.total_exits++;
    { extern uint32_t g_hv_reason_hist[80]; g_hv_reason_hist[(vcpu->exit_reason & 0xFFFF) % 80]++; }
    // trace the first exits so a bring-up failure names its exit (satoru)
    static uint32_t traced = 0;
    if (stats.total_exits <= 1) traced = 0;
    if (traced < 300 && (vcpu->exit_reason & 0xFFFF) != 1 && (vcpu->exit_reason & 0xFFFF) != 30 && (vcpu->exit_reason & 0xFFFF) != 12 && vcpu->type == VIRT_INTEL_VTX) {
        traced++;
        SerialLogger::Log("[vmx] exit#");
        SerialLogger::LogDec((int)stats.total_exits);
        SerialLogger::Log(" reason=0x");
        SerialLogger::LogHex(vcpu->exit_reason);
        SerialLogger::Log(" qual=0x");
        SerialLogger::LogHex(vcpu->exit_qualification);
        SerialLogger::Log(" rip=0x");
        SerialLogger::LogHex64(VMM::VMRead(VMCS_GUEST_RIP));
        SerialLogger::Log(" rax=0x");
        SerialLogger::LogHex64(vcpu->regs[0]);
        SerialLogger::Log(" rsi=0x"); SerialLogger::LogHex64(vcpu->regs[6]); SerialLogger::Log(" rbp=0x"); SerialLogger::LogHex64(vcpu->regs[5]);
        SerialLogger::Log(" apic=");
        SerialLogger::LogDec((int)HVApicId());
        SerialLogger::Log("\r\n");
    }

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

//  handleguestio - route guest i/o to virtual devices

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

    // port 0x80 - post code (ignore)
    if (port == 0x80) return true;

    // ports 0xcf8 / 0xcfc-0xcff - virtual pci configuration mechanism #1
    if (port == 0xCF8 || (port >= 0xCFC && port <= 0xCFF)) {
        return VPCI::HandlePortIO(port, is_out, size, value);
    }

    // port 0x92 (system control port a - a20 gate)
    if (port == 0x92) {
        if (!is_out) value = 0x02; // a20 enabled
        return true;
    }

    // port 0x70/0x71 - cmos/rtc
    if (port == 0x70 || port == 0x71) {
        if (!is_out) value = 0;
        return true;
    }

    // reboot: keyboard controller pulse (0x64 <- 0xfe) or the reset control
    // register (0xcf9 <- 0x06/0x0e), linux's acpi=off reboot paths (satoru)
    if (port == 0x64 && is_out && (value & 0xFF) == 0xFE) { NoteRebootRequest(); return true; }
    if (port == 0xCF9 && is_out && (value & 0x04)) { NoteRebootRequest(); return true; }
    if (port == 0xCF9) { if (!is_out) value = 0; return true; }

    // a multiboot2 (kurono) guest probes a real 8042 (satoru)
    if (i8042_on && VI8042::HandlePort(port, is_out, value)) return true;

    // unhandled
    return false;
}


static uint64_t g_disk_virt = 0;
void Hypervisor::SetDiskVirtualGb(uint32_t gb) { g_disk_virt = (uint64_t)gb << 30; }

//  smp guests: vcpu bookkeeping, mp table, init / sipi (satoru)

extern "C" uint8_t g_vmx_guest_fx[512];
namespace {
    // what is left to swap per vcpu when they take turns on one cpu: cr2 (not in the vmcs).
    // the fx image, the switched msrs and the debug registers live in each vCPU now (satoru)
    struct VAux { uint64_t cr2; };
    VAux g_aux[8];
}

void Hypervisor::SetGuestCpus(int n) { want_cpus = n < 1 ? 1 : (n > 8 ? 8 : n); }
int  Hypervisor::GuestCpus() { return nvcpus; }
int  Hypervisor::CurrentVcpu() { return cur_vcpu; }

void Hypervisor::SwitchVcpu(int k) {
    if (k < 0 || k >= nvcpus || !vcpus[k]) return;
    if (k != cur_vcpu) {
        asm volatile("mov %%cr2, %0" : "=r"(g_aux[cur_vcpu].cr2));        // cr2 is not in the vmcs: a guest page fault's address would be lost across a switch (satoru)
        asm volatile("mov %0, %%cr2" : : "r"(g_aux[k].cr2) : "memory");
        cur_vcpu = k;
        vcpu = vcpus[k];
    }
    VMM::SetCurrent(vcpu);
    VMM::VMPtrLoad(vcpu->vmcs);
}

void Hypervisor::SetupAPs() {
    if (g_par_on) return;                       // each ap thread builds its vmcs on its own cpu (satoru)
    for (int i = 0; i < 8; i++) g_aux[i].cr2 = 0;
    for (int k = 1; k < nvcpus; k++) {
        if (!VMM::SetupVCPU(vcpus[k])) { SerialLogger::Log("Hypervisor: AP setup failed\r\n"); nvcpus = k; break; }
        if (ept_root) VMM::VMWrite(VMCS_EPT_POINTER, EPTManager::BuildEPTP(ept_root));
        if (io_bitmap_a) VMM::VMWrite(VMCS_IO_BITMAP_A, KernelPhysOf(io_bitmap_a));
        if (io_bitmap_b) VMM::VMWrite(VMCS_IO_BITMAP_B, KernelPhysOf(io_bitmap_b));
        if (msr_bitmap) VMM::VMWrite(VMCS_MSR_BITMAP_ADDR, KernelPhysOf(msr_bitmap));
    }
    if (nvcpus > 1) {
        // vmx preemption timer: every vcpu exits about every 100 us, so lapic timers fire on
        // time and a spinning vcpu cannot hold the thread for a whole host tick (satoru)
        uint64_t basic = VMM::ReadMSR(0x480);
        uint64_t pin = VMM::ReadMSR(((basic >> 55) & 1) ? 0x48D : 0x481);
        uint32_t rate = (uint32_t)(VMM::ReadMSR(0x485) & 0x1F);
        uint64_t tsc = CPUDetect::GetInfo().frequency.tsc_frequency;
        if (tsc < 1000000ULL) tsc = 2000000000ULL;
        uint64_t ticks = (tsc / 10000) >> rate;
        if (ticks < 1) ticks = 1;
        if ((pin >> 32) & PIN_PREEMPT_TIMER) {
            for (int k = 0; k < nvcpus; k++) {
                VMM::VMPtrLoad(vcpus[k]->vmcs);
                VMM::VMWrite(VMCS_PIN_BASED_CONTROLS, VMM::VMRead(VMCS_PIN_BASED_CONTROLS) | PIN_PREEMPT_TIMER);
                VMM::VMWrite(0x482E, ticks);
            }
            SerialLogger::Log("[smp] preemption timer on\r\n");
        }
        VMM::VMPtrLoad(vcpus[0]->vmcs);
    }
}

void Hypervisor::StartAP(int k, uint8_t vector) {
    if (k <= 0 || k >= nvcpus || ap_state[k]) return;
    int back = cur_vcpu;
    SwitchVcpu(k);
    VMM::SetupVMCSGuestState(vcpu);                         // real mode power-on state
    {   // same cr0 / cr4 ownership as the bsp (sdm 26.3.1.1: guest cr4 must carry vmxe) (satoru)
        uint64_t f0 = 0, f1 = ~0ULL;
        VMM::SafeReadMSR(MSR_IA32_VMX_CR0_FIXED0, f0);
        VMM::VMWrite(VMCS_CR0_GUEST_HOST_MASK, f0 & ~((1ULL << 0) | (1ULL << 31)));
        VMM::SafeReadMSR(MSR_IA32_VMX_CR4_FIXED0, f0);
        VMM::SafeReadMSR(MSR_IA32_VMX_CR4_FIXED1, f1);
        VMM::VMWrite(VMCS_GUEST_CR4, f0 & f1);
        VMM::VMWrite(VMCS_CR4_GUEST_HOST_MASK, f0 | ~f1);
        VMM::VMWrite(VMCS_CR4_READ_SHADOW, 0);
    }
    uint64_t seg = (uint64_t)vector << 8;
    VMM::VMWrite(VMCS_GUEST_CS_SEL, seg);
    VMM::VMWrite(VMCS_GUEST_CS_BASE, seg << 4);
    VMM::VMWrite(VMCS_GUEST_RIP, 0);
    for (int i = 0; i < 16; i++) vcpu->regs[i] = 0;
    for (int i = 0; i < 5; i++) vcpu->sw_msr[i] = 0;
    for (int i = 0; i < 8; i++) vcpu->dr[i] = 0;
    vcpu->dr[6] = 0xFFFF0FF0ULL; vcpu->dr[7] = 0x400ULL;
    for (int i = 0; i < 512; i++) vcpu->fx[i] = g_vmx_guest_fx[i];
    g_aux[k].cr2 = 0;
    ap_state[k] = 1; vhalted[k] = false;
    SerialLogger::Log("[smp] vcpu "); SerialLogger::LogDec(k); SerialLogger::Log(" started at page 0x");
    SerialLogger::LogHex(vector); SerialLogger::Log("\r\n");
    SwitchVcpu(back);
}

void Hypervisor::DumpSmp() {
    if (nvcpus < 2) return;
    int back = cur_vcpu;
    for (int k = 0; k < nvcpus; k++) {
        SwitchVcpu(k);
        SerialLogger::Log("[smp] vcpu "); SerialLogger::LogDec(k);
        SerialLogger::Log(ap_state[k] ? (vhalted[k] ? " halted" : " running") : " parked");
        SerialLogger::Log(" rip=0x"); SerialLogger::LogHex64(VMM::VMRead(VMCS_GUEST_RIP));
        SerialLogger::Log(" cs=0x"); SerialLogger::LogHex64(VMM::VMRead(VMCS_GUEST_CS_SEL));
        SerialLogger::Log(" cr0=0x"); SerialLogger::LogHex64(VMM::VMRead(VMCS_GUEST_CR0));
        SerialLogger::Log(" rfl=0x"); SerialLogger::LogHex64(VMM::VMRead(VMCS_GUEST_RFLAGS));
        SerialLogger::Log(" gsb=0x"); SerialLogger::LogHex64(VMM::VMRead(VMCS_GUEST_GS_BASE));
        SerialLogger::Log(" exit="); SerialLogger::LogDec((int)(vcpu->exit_reason & 0xFFFF));
        SerialLogger::Log("\r\n");
    }
    SwitchVcpu(back);
    VLapic::DumpCounts();
}

void Hypervisor::LapicIpi(int target, int mode, uint8_t vector) {
    if (target <= 0 || target >= nvcpus) return;
    if (g_par_on) {                                         // the ap thread starts itself (satoru)
        if (mode == 6 && g_ap_start[target] == 0 && ap_state[target] == 0) { g_ap_vec[target] = vector; __atomic_store_n(&g_ap_start[target], (uint8_t)1, __ATOMIC_RELEASE); KickVcpu(target); }
        return;
    }
    if (mode == 5) {                                        // init: park the cpu until a sipi
        if (ap_state[target]) SerialLogger::Log("[smp] init ipi\r\n");
        ap_state[target] = 0; vhalted[target] = false;
        VLapic::ResetOne(target);
    } else if (mode == 6) {
        StartAP(target, vector);
    }
}

// intel mp table in the bios area (0xf0000..): lapics only, pic mode (imcr present) (satoru)
void Hypervisor::BuildMpTable(int n) {
    uint8_t* rom = GuestMemoryManager::GetROMArea();
    if (!rom) return;
    uint8_t* cfg = rom + (0xF0100 - GUEST_ROM_BASE);
    uint8_t* mpf = rom + (0xF0010 - GUEST_ROM_BASE);
    uint32_t sig, d0, d1, d2;
    asm volatile("cpuid" : "=a"(sig), "=b"(d0), "=c"(d1), "=d"(d2) : "a"(1), "c"(0));
    memset(cfg, 0, 512);
    memcpy(cfg, "PCMP", 4);
    cfg[6] = 4;
    memcpy(cfg + 8, "KURONO  ", 8);
    memcpy(cfg + 16, "KURONO VM   ", 12);
    uint32_t lapic = 0xFEE00000; memcpy(cfg + 36, &lapic, 4);
    uint8_t* e = cfg + 44;
    for (int c = 0; c < n; c++) {
        e[0] = 0; e[1] = (uint8_t)c; e[2] = 0x14; e[3] = (uint8_t)(1 | (c == 0 ? 2 : 0));
        memcpy(e + 4, &sig, 4); memcpy(e + 8, &d2, 4);
        e += 20;
    }
    e[0] = 1; e[1] = 0; memcpy(e + 2, "ISA   ", 6); e += 8;
    uint16_t cnt = (uint16_t)(n + 1), len = (uint16_t)(e - cfg);
    memcpy(cfg + 34, &cnt, 2); memcpy(cfg + 4, &len, 2);
    uint8_t sum = 0; for (int i = 0; i < len; i++) sum = (uint8_t)(sum + cfg[i]);
    cfg[7] = (uint8_t)(0 - sum);
    memset(mpf, 0, 16);
    memcpy(mpf, "_MP_", 4);
    uint32_t pp = 0xF0100; memcpy(mpf + 4, &pp, 4);
    mpf[8] = 1; mpf[9] = 4; mpf[11] = 0; mpf[12] = 0x80;     // feature1 0 = table, feature2 bit 7 = imcr
    sum = 0; for (int i = 0; i < 16; i++) sum = (uint8_t)(sum + mpf[i]);
    mpf[10] = (uint8_t)(0 - sum);
}

//  handleguestmmio - route mmio to virtual devices

bool Hypervisor::HandleGuestMMIO(uint64_t phys_addr, bool is_write,
                                   uint8_t size, uint32_t& value) {
    stats.mmio_exits++;
    if (VPCI::HandleMMIO(phys_addr, is_write, size, value)) return true;
    return VirtualDevices::HandleMMIO(phys_addr, is_write, size, value);
}

//  interrupt injection - inject interrupts/exceptions into guest

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
    // the old code acked the pic first and dropped the irq when the guest had
    // if=0, so the guest lost most timer ticks. peek first; if the guest
    // cannot take it, ask for an interrupt-window exit (sdm 26.7.5) (satoru)
    if (vcpu && vcpu->type == VIRT_INTEL_VTX) {
        VirtualPIC& m = VirtualDevices::GetMasterPIC();
        // smp: the pic feeds the bsp only (virtual wire); every vcpu has a lapic (satoru)
        bool bsp = (cur_vcpu == 0);
        // a one-vcpu kurono guest still runs its tick on the lapic timer
        // (SMP::StartBspLapicTick): the old single-vcpu VirtualAPIC never
        // delivered it, so the guest got no timer once the 8259 tick stopped
        // (satoru)
        int lv = (nvcpus > 1 || mb2_guest) ? VLapic::Pending(cur_vcpu) : -1;
        bool pending = false;
        if (bsp) {
            // cascade: a pending slave request drives master line 2, which
            // nothing did before, so irqs 8-15 were never delivered (satoru)
            if (VirtualDevices::GetSlavePIC().GetHighestPriorityIRQ() >= 0) m.SetIRQ(2, true);
            int hi = m.GetHighestPriorityIRQ();
            pending = hi >= 0 && (hi != 2 || VirtualDevices::GetSlavePIC().GetHighestPriorityIRQ() >= 0);
        }
        if (pending || lv >= 0) {
            VMM::VMPtrLoad(vcpu->vmcs);
            uint64_t rflags = VMM::VMRead(VMCS_GUEST_RFLAGS);
            uint64_t intr = VMM::VMRead(VMCS_GUEST_INTERRUPTIBILITY);
            uint64_t inj = VMM::VMRead(VMCS_ENTRY_INT_INFO);
            bool can = (rflags & (1 << 9)) && !(intr & 3) && !(inj & (1u << 31));
            if (!can) {
                if (!(inj & (1u << 31)))
                    VMM::VMWrite(VMCS_PROC_BASED_CONTROLS,
                                 VMM::VMRead(VMCS_PROC_BASED_CONTROLS) | PROC_INT_WINDOW_EXIT);
            } else {
                int irq = pending ? VirtualDevices::GetPendingIRQ() : -1;
                if (irq >= 0) InjectInterrupt((uint8_t)irq);
                else if (lv >= 0 && InjectInterrupt((uint8_t)lv)) VLapic::Accept(cur_vcpu, lv);
            }
        }
    } else {
        int irq = VirtualDevices::GetPendingIRQ();
        if (irq >= 0) {
            InjectInterrupt((uint8_t)irq);
        }
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

//  tickdevices - advance virtual device emulation

void Hypervisor::TickDevices() {
    stats.tick_count++;
    uint32_t us = config.timer_tick_us;

    VirtualDevices::Tick(us);

    if (config.enable_serial) {
        serial.Tick(us);
    }

    // virtio-gpu host: pull pending requests off the controlq even if the
    // guest forgot to ring the doorbell, then present any dirty scanout.
    VirtIOGPUHost::ProcessQueues();
    VirtIOGPUHost::PresentIfDirty();
    VirtioPollAll();    // doorbell-less work from the guest (satoru)
    if (i8042_on) VI8042::Tick();   // host-typed keys -> irq1 (satoru)
    if (mb2_guest) PCIPassthrough::TickIntx();   // passed-through INTx -> guest (satoru)
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

//  serial command bridge - host → guest via virtual com1

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

//  bootalpine - one-call boot of embedded alpine linux guest vm

bool Hypervisor::BootAlpine() {
    if (IsOwned()) return false;   // the background vm host has the vm (satoru)

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
                        "nolapic nosmp noacpi"; // (satoru)

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

    SerialLogger::Log("Hypervisor: Alpine VM configured - entering run loop\r\n");
    VMState final_state = RunVM(0); // run until halt/crash

    SerialLogger::Log("Hypervisor: Alpine VM exited with state ");
    SerialLogger::LogDec(final_state);
    SerialLogger::Log("\r\n");

    return (final_state != VM_STATE_CRASHED);
}

// with no console up yet, the guest's printk text still sits in its ram.
// find "Linux version" in guest memory and print the readable text after it,
// so an early guest panic is visible in the host log (satoru)
static void DumpGuestPrintkText() {
    uint8_t* ram = GuestMemoryManager::GetHighRAM();
    uint32_t sz = GuestMemoryManager::GetHighRAMSize();
    if (sz > GuestMemoryManager::ChunkBytes()) sz = GuestMemoryManager::ChunkBytes();   // chunk 0 is all that is contiguous (satoru)
    if (!ram || sz < 64) return;
    static const char pat[] = "Linux version 6";
    const uint32_t pl = sizeof(pat) - 1;
    int hits = 0;
    for (uint32_t i = 0; i + pl < sz; i++) {
        if (ram[i] != 'L' || memcmp(ram + i, pat, pl) != 0) continue;
        // the first hit is usually linux_banner in .rodata, the log copy
        // comes later; dump up to four hits (satoru)
        if (++hits > 4) break;
        SerialLogger::Log("[guestlog] at gpa 0x");
        SerialLogger::LogHex64((uint64_t)i + GUEST_HIGH_RAM_START);
        SerialLogger::Log(":\r\n");
        char line[160];
        int ln = 0, printed = 0;
        bool gap = false;
        for (uint32_t j = i; j < sz && printed < 5000; j++) {
            uint8_t c = ram[j];
            if (c >= 0x20 && c < 0x7F) {
                if (gap && ln > 0) {
                    line[ln] = 0;
                    SerialLogger::Log("[guestlog] ");
                    SerialLogger::Log(line);
                    SerialLogger::Log("\r\n");
                    ln = 0;
                }
                gap = false;
                line[ln++] = (char)c;
                printed++;
                if (ln >= (int)sizeof(line) - 1) {
                    line[ln] = 0;
                    SerialLogger::Log("[guestlog] ");
                    SerialLogger::Log(line);
                    SerialLogger::Log("\r\n");
                    ln = 0;
                }
            } else {
                gap = true;
            }
        }
        i += pl;
    }
    SerialLogger::Log("[guestlog] scan done\r\n");
}

//  bootalpinewithextraction - boot alpine, capture boot log, extract drivers

bool Hypervisor::BootAlpineWithExtraction(uint32_t max_boot_exits) {
    if (IsOwned()) return false;   // (satoru)

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
    // re-read hw_available - init() detects svm/vt-x and may update it
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
                            "nolapic nosmp noacpi"; // (satoru)

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
                    // keep running until the guest reaches a shell prompt or
                    // panics, not just until its first serial byte (satoru)
                    g_guest_tail_len = 0;
                    g_guest_prompt_seen = false;
                    g_guest_panic_seen = false;
                    uint32_t extra_budget = max_boot_exits;
                    if (extra_budget < 6000000) extra_budget = 6000000;
                    uint32_t used_budget = 0;
                    while ((st == VM_STATE_RUNNING || st == VM_STATE_PAUSED) &&
                           !g_guest_prompt_seen && !g_guest_panic_seen &&
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
                    if (!g_guest_prompt_seen) DumpGuestPrintkText(); // (satoru)

                    if (st == VM_STATE_RUNNING || st == VM_STATE_PAUSED) {
                        alpine_booted = true;
                        hw_ok = true;
                    }
                    if (alpine_boot_log_len > 0) {
                        alpine_booted = true;
                        hw_ok = true;
                    }

                    // under whpx, vm entry failure (crashed state) means
                    // nested virt truly isn't supported - fall back cleanly
                    if (st == VM_STATE_CRASHED && VMM::IsWHPX()) {
                        SerialLogger::Log("Hypervisor: WHPX VM entry failed - "
                                         "nested virt not supported by host\r\n");
                        hw_ok = false;
                    }
                }
            }
        }

        if (hw_ok) {
            // hardware boot succeeded - extract drivers and return
            int drv_count = ExtractAlpineDrivers();
            _auto_setup_alpine_userland();
            SerialLogger::Log("Hypervisor: Extracted ");
            SerialLogger::LogDec(drv_count);
            SerialLogger::Log(" drivers from Alpine (HW mode)\r\n");
            return true;
        }

        SerialLogger::Log("Hypervisor: HW Alpine boot failed - state=");
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
            SerialLogger::Log(" - WHPX doesn't support nested virt\r\n");
            SerialLogger::Log("Hypervisor: QEMU's WHPX accelerator cannot emulate vmrun/vmlaunch.\r\n");
            SerialLogger::Log("Hypervisor: To get real HW Alpine, use KVM (Linux) or bare metal.\r\n");
        } else {
            SerialLogger::Log("\r\n");
        }
        SerialLogger::Log("Hypervisor: Alpine requires bare-metal AMD-V or VT-x. No fallback.\r\n");

        // write diagnostic into boot log
        if (alpine_boot_log_len == 0) {
            const char* diag =
                "[kurono] Alpine boot failed - no hardware virtualization.\n"
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

    // no software fallback - honest failure beats fake success
    return false;
}

//  bootdebianwithextraction - boot embedded debian ext4 rootfs guest

static void PumpGuestConsole(VirtualSerial& ser, char* keep, int& keep_len, int keep_max);

void Hypervisor::SetOwner(void* proc) { owner_proc = proc; }

void Hypervisor::GuestIrqState(uint64_t* rf, uint64_t* in, uint64_t* ij, uint64_t* pc) {
    *rf = *in = *ij = *pc = 0;
    if (!vcpu || vcpu->type != VIRT_INTEL_VTX || ForeignCaller()) return;
    if (vm_state != VM_STATE_RUNNING && vm_state != VM_STATE_PAUSED) return;
    *rf = VMM::VMRead(VMCS_GUEST_RFLAGS);
    *in = VMM::VMRead(VMCS_GUEST_INTERRUPTIBILITY);
    *ij = VMM::VMRead(VMCS_ENTRY_INT_INFO);
    *pc = VMM::VMRead(VMCS_PROC_BASED_CONTROLS);
}

uint64_t Hypervisor::GuestRIP() {
    if (!vcpu || vcpu->type != VIRT_INTEL_VTX || ForeignCaller()) return 0;
    if (vm_state != VM_STATE_RUNNING && vm_state != VM_STATE_PAUSED) return 0;
    return VMM::VMRead(VMCS_GUEST_RIP);
}

// true when the vm belongs to the background host and the caller is not its
// thread: that caller must not touch the vmcs (it lives on the host's cpu and
// is in the middle of a run) (satoru)
bool Hypervisor::ForeignCaller() {
    if (!owner_proc) return false;
    return (void*)Scheduler::GetCurrentProcess() != owner_proc;
}

bool Hypervisor::PrepareLinuxGuest(const LinuxGuestSpec& spec) {
    if (!spec.kernel || !spec.kernel_size || !spec.initrd || !spec.initrd_size) {
        SerialLogger::Log("Hypervisor: guest kernel / initramfs missing\r\n");
        return false;
    }
    guest_halted = false;
    reboot_requested = false;
    if (vm_state != VM_STATE_UNINITIALIZED && vm_state != VM_STATE_DESTROYED) DestroyVM();
    Init();
    hw_available = VMM::IsSupported();
    if (!hw_available) {
        SerialLogger::Log("Hypervisor: linux guests need vt-x\r\n");
        return false;
    }
    VMConfig cfg;
    cfg.SetDefaults();
    cfg.ram_mb        = spec.ram_mb ? spec.ram_mb : 1024;
    cfg.disk_size_mb  = 0;
    cfg.enable_serial = true;     // com1 stays for earlyprintk / debug consoles
    cfg.enable_disk   = false;
    cfg.enable_apic   = true;
    cfg.enable_gpu    = spec.gpu;
    cfg.timer_tick_us = 1000;
    cfg.cmdline       = spec.cmdline;
    if (!CreateVM(cfg)) { SerialLogger::Log("Hypervisor: CreateVM failed\r\n"); return false; }
    if (GuestMemoryManager::GetTotalRAM() < (cfg.ram_mb - 1) * 1024u * 1024u) {
        SerialLogger::Log("Hypervisor: not enough host memory for the guest\r\n");
        DestroyVM();
        return false;
    }
    // virtio devices, one legacy pic line each (satoru)
    GuestConsole::ResetRings(0);
    GuestConsole::ResetRings(1);
    GuestConsole::ResetRings(2);
    GuestConsole::ResetRings(3);
    GuestConsole::Init(0, 5, "virtio-console");
    GuestConsole::Init(1, 3, "virtio-console-agent");
    GuestConsole::Init(2, 11, "virtio-console-wayland");   // wayland bridge channel (satoru)
    GuestConsole::Init(3, 7, "virtio-console-audio");      // guest pulse pcm channel (satoru)
    if (spec.disk_bytes) {
        // dynamic mode: the guest sees a bigger disk than the image; the rest is chunk files (satoru)
        bool deb = (linux_guest_profile == LINUX_GUEST_DEBIAN);
        VBlk::Init(spec.disk_lba, spec.disk_bytes, 10, spec.disk_id,
                   deb ? "DEBIAN  " : "ALPINE  ",
                   deb ? "DEBIAN  IMG" : "ALPINE  IMG", g_disk_virt);
    }
    if (spec.net) VNet::Init(9);
    if (GuestMemoryManager::IsLazy()) VBalloon::Init(6);     // dynamic mode: free page reporting (satoru)
    if (!LoadLinuxKernel(spec.kernel, spec.kernel_size, spec.cmdline) || !LoadInitrd(spec.initrd, spec.initrd_size)) {
        SerialLogger::Log("Hypervisor: failed to load the guest kernel / initramfs\r\n");
        DestroyVM();
        return false;
    }
    // first entry: vmcs setup + protected-mode entry state; one exit leaves
    // the vm RUNNING for RunSlice (satoru)
    RunVM(1);
    return vm_state == VM_STATE_RUNNING || vm_state == VM_STATE_PAUSED;
}

bool Hypervisor::PrepareMultiboot2Guest(const uint8_t* elf, uint64_t size, const char* cmdline, uint32_t ram_mb) {
    guest_halted = false;
    reboot_requested = false;
    mb2_mode = false;
    if (vm_state != VM_STATE_UNINITIALIZED && vm_state != VM_STATE_DESTROYED) DestroyVM();
    Init();
    hw_available = VMM::IsSupported();
    if (!hw_available) { SerialLogger::Log("Hypervisor: a multiboot2 guest needs vt-x\r\n"); return false; }
    VMConfig cfg;
    cfg.SetDefaults();
    cfg.ram_mb        = ram_mb ? ram_mb : 1024;
    cfg.disk_size_mb  = 0;
    cfg.enable_serial = true;     // the guest's whole log comes out of com1 (satoru)
    cfg.enable_disk   = false;
    cfg.enable_apic   = true;
    cfg.enable_gpu    = false;
    cfg.timer_tick_us = 1000;
    cfg.cmdline       = cmdline;
    if (!CreateVM(cfg)) { SerialLogger::Log("Hypervisor: CreateVM failed\r\n"); return false; }
    if (GuestMemoryManager::GetTotalRAM() < (cfg.ram_mb - 1) * 1024u * 1024u) {
        SerialLogger::Log("Hypervisor: not enough host memory for the guest\r\n");
        DestroyVM();
        return false;
    }
    VI8042::Reset();
    i8042_on = true;
    mb2_guest = true;
    // kurono.vm.pt devices (satoru)
    if (PCIPassthrough::ReservedCount()) {
        int n = PCIPassthrough::AttachReserved(ept_root);
        SerialLogger::Log("Hypervisor: passthrough devices attached: ");
        SerialLogger::LogDec(n);
        SerialLogger::Log("\r\n");
    }
    uint32_t entry = 0, info = 0;
    if (!MB2Boot::Load(elf, size, cmdline, &entry, &info)) {
        SerialLogger::Log("Hypervisor: multiboot2 load failed: ");
        SerialLogger::Log(MB2Boot::Error());
        SerialLogger::Log("\r\n");
        DestroyVM();
        return false;
    }
    mb2_entry = entry; mb2_info = info; mb2_mode = true;
    RunVM(1);
    mb2_mode = false;
    return vm_state == VM_STATE_RUNNING || vm_state == VM_STATE_PAUSED;
}

bool Hypervisor::DebianPrepare(uint32_t ram_mb, bool debug) {
    // debian's own cloud kernel + a small initramfs (DEBKERN.BIN / DEBINIT.BIN
    // on the kurono partition); the rootfs DEBIAN.IMG behind virtio-blk. (satoru)
    static const char deb83[11] = { 'D','E','B','I','A','N',' ',' ','I','M','G' };
    LinuxGuestSpec sp = {};
    sp.name = "debian";
    sp.kernel = KAsset::Data("DEBKERN BIN", nullptr, nullptr);
    sp.kernel_size = KAsset::Size("DEBKERN BIN", nullptr, nullptr);
    sp.initrd = KAsset::Data("DEBINIT BIN", nullptr, nullptr);
    sp.initrd_size = KAsset::Size("DEBINIT BIN", nullptr, nullptr);
    if (!sp.kernel || !sp.kernel_size || !sp.initrd || !sp.initrd_size) {
        SerialLogger::Log("Hypervisor: DEBKERN.BIN / DEBINIT.BIN missing from the KURONO partition\r\n");
        return false;
    }
    sp.cmdline = debug
        ? "console=ttyS0 console=hvc0 rdinit=/init kroot=/dev/vda loglevel=7 nokaslr noapic nolapic nosmp acpi=off panic=5"
        : "console=hvc0 rdinit=/init kroot=/dev/vda loglevel=4 nokaslr noapic nolapic nosmp acpi=off panic=5";
    sp.ram_mb = ram_mb ? ram_mb : 768;
    if (!DiskLog::ArmGuestImage(deb83, &sp.disk_lba, &sp.disk_bytes)) {
        SerialLogger::Log("Hypervisor: DEBIAN.IMG missing or fragmented in the KURONO partition\r\n");
        return false;
    }
    sp.disk_id = "kurono-debian";
    sp.net = true;
    sp.gpu = false;
    debian_boot_log_len = 0;
    debian_boot_log[0] = 0;
    debian_booted = false;
    linux_guest_profile = LINUX_GUEST_DEBIAN;
    return PrepareLinuxGuest(sp);
}

static bool GuestIrqPending() {
    return VirtualDevices::GetMasterPIC().GetHighestPriorityIRQ() >= 0 ||
           VirtualDevices::GetSlavePIC().GetHighestPriorityIRQ() >= 0;
}

// real-time device ticking: the guest's timers advance by elapsed host time
// (kvm-clock's tsc base), and the virtio devices are serviced (satoru)
static uint64_t g_slice_last_us = 0;

VMState Hypervisor::RunSlice(uint32_t budget_us, bool* idle) {
    if (idle) *idle = false;
    if (vm_state != VM_STATE_RUNNING && vm_state != VM_STATE_PAUSED) return vm_state;
    vm_state = VM_STATE_RUNNING;
    uint64_t t0 = VMExitHandler::GuestClockNs() / 1000;
    if (g_slice_last_us == 0 || t0 < g_slice_last_us) g_slice_last_us = t0;
    if (nvcpus > 1 && g_par_on) return RunSlicePar(budget_us, idle);
    if (nvcpus > 1) return RunSliceSmp(budget_us, idle);
    for (;;) {
        uint64_t now = VMExitHandler::GuestClockNs() / 1000;
        if (now - g_slice_last_us >= 250) {
            uint64_t d = now - g_slice_last_us;
            if (d > 100000) d = 100000;             // do not replay a long stall as a tick storm
            g_slice_last_us = now;
            stats.tick_count++;
            VirtualDevices::Tick((uint32_t)d);
            if (config.enable_serial) serial.Tick((uint32_t)d);
            if (config.enable_gpu) {
                VirtIOGPUHost::ProcessQueues();
                VirtIOGPUHost::PresentIfDirty();
            }
            GuestConsole::Poll(0);
            GuestConsole::Poll(1);
            GuestConsole::Poll(2);
            GuestConsole::Poll(3);
            VNet::Poll();
            VirtioPollAll();
        }
        // a kurono guest ticks from its lapic timer (VLapic, see vdevices
        // HandleMMIO): fire it here and let it wake a halted guest (satoru)
        if (mb2_guest) VLapic::Poll(VMExitHandler::GuestClockNs());
        if (guest_halted) {
            if (!GuestIrqPending() && !(mb2_guest && VLapic::Pending(0) >= 0)) {
                // hlt with interrupts off never wakes: linux's poweroff path
                // (no acpi here) ends exactly like that (satoru)
                if (vcpu && vcpu->type == VIRT_INTEL_VTX &&
                    !(VMM::VMRead(VMCS_GUEST_RFLAGS) & (1u << 9))) {
                    vm_state = VM_STATE_HALTED;
                    break;
                }
                if (idle) *idle = true;
                break;
            }
            guest_halted = false;
        }
        vm_state = RunOneCycle();
        if (reboot_requested) { reboot_requested = false; vm_state = VM_STATE_REBOOTING; }
        if (vm_state != VM_STATE_RUNNING) break;
        if (VMExitHandler::GuestClockNs() / 1000 - t0 >= budget_us) break;
    }
    return vm_state;
}

// ---- parallel vcpus (satoru)
// the bsp stays on the vm host thread; every other vcpu gets a kernel thread of its own on a service
// cpu. one big vm lock (g_vlock) covers every exit handler, interrupt injection and the device pumps;
// the guest itself runs outside it. lock order: kls (only for exits that reach the device models)
// first, then g_vlock. the order is never reversed. (satoru)
static volatile uint32_t g_vlock = 0;
static inline void vlock() { while (__atomic_exchange_n(&g_vlock, 1u, __ATOMIC_ACQUIRE)) while (g_vlock) __asm__ volatile("pause"); }
static inline void vunlock() { __atomic_store_n(&g_vlock, 0u, __ATOMIC_RELEASE); }
static volatile bool g_par_run = false;       // ap threads may run guests
static uint32_t g_vcpu_cpu[8];
static uint32_t g_bsp_cpu = 0;
static volatile bool g_ap_spawned[8];
static volatile uint32_t g_sync_req = 0, g_sync_ready = 0, g_sync_done = 0, g_sync_ping = 0, g_sync_pong = 0;
static volatile uint64_t g_sync_t2 = 0;
static volatile uint32_t g_sync_owner = 0;
static inline uint64_t rdtsc_raw() { uint32_t lo, hi; asm volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((uint64_t)hi << 32) | lo; }

bool Hypervisor::ParActive() { return g_par_on; }
void Hypervisor::ParLock() { if (g_par_on) vlock(); }
void Hypervisor::ParUnlock() { if (g_par_on) vunlock(); }
void Hypervisor::ParSetCtx(int k) { cur_vcpu = k; vcpu = vcpus[k]; VMM::SetCurrent(vcpu); }

void Hypervisor::KickVcpu(int c) {
    if (!g_par_on || c < 0 || c >= nvcpus) return;
    uint32_t cpu = c == 0 ? g_bsp_cpu : g_vcpu_cpu[c];
    if (cpu && cpu != SMP::CpuIndex()) SMP::KickCpu(cpu);
    else if (!cpu && c == 0 && SMP::CpuIndex() != g_bsp_cpu) SMP::KickCpu(g_bsp_cpu);
}

// the bsp side of the tsc handshake: answer pings in a tight loop while an ap measures (satoru)
static void par_serve_sync() {
    if (!g_sync_req) return;
    g_sync_ready = 1;
    uint32_t last = 0;
    uint64_t guard = rdtsc_raw() + 400000000ULL;
    while (!g_sync_done && rdtsc_raw() < guard) {
        uint32_t p = g_sync_ping;
        if (p != last) { g_sync_t2 = rdtsc_raw(); __atomic_store_n(&g_sync_pong, p, __ATOMIC_RELEASE); last = p; }
        __asm__ volatile("pause");
    }
    g_sync_ready = 0; g_sync_req = 0;
}

// the ap side: offset so that this cpu's tsc + offset is the bsp cpu's tsc (satoru)
static int64_t par_tsc_offset() {
    while (__atomic_exchange_n(&g_sync_owner, 1u, __ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    g_sync_done = 0; g_sync_ping = 0; g_sync_pong = 0;
    g_sync_req = 1;
    uint64_t wait_until = rdtsc_raw() + 2000000000ULL;
    while (!g_sync_ready && rdtsc_raw() < wait_until) __asm__ volatile("pause");
    int64_t best_off = 0; uint64_t best_rtt = ~0ULL;
    if (g_sync_ready) {
        for (uint32_t i = 1; i <= 64; i++) {
            uint64_t t1 = rdtsc_raw();
            __atomic_store_n(&g_sync_ping, i, __ATOMIC_RELEASE);
            uint64_t to = t1 + 40000000ULL;
            while (g_sync_pong != i && rdtsc_raw() < to) __asm__ volatile("pause");
            uint64_t t4 = rdtsc_raw();
            if (g_sync_pong != i) break;
            uint64_t t2 = g_sync_t2;
            uint64_t rtt = t4 - t1;
            if (rtt < best_rtt) { best_rtt = rtt; best_off = (int64_t)t2 - (int64_t)((t1 >> 1) + (t4 >> 1)); }
        }
    }
    g_sync_done = 1;
    __atomic_store_n(&g_sync_owner, 0u, __ATOMIC_RELEASE);
    return best_off;
}

static bool par_exit_needs_kls(vCPU* c) {
    uint32_t r = c->exit_reason & 0xFFFF;
    if (r == 30 || r == 18) return true;                     // port io, vmcall
    if (r == 48 || r == 49) {
        uint64_t gpa = VMM::VMRead(VMCS_GUEST_PHYS_ADDR);
        if (gpa >= GUEST_LAPIC_BASE && gpa < GUEST_LAPIC_BASE + 0x1000) return false;
        if (gpa < (uint64_t)GUEST_HIGH_RAM_START + GuestMemoryManager::GetHighRAMSize()) return false;   // ram (lazy backing)
        return true;
    }
    return false;
}

static void par_idle_hlt() { __asm__ volatile("sti; hlt" ::: "memory"); }

void Hypervisor::ApMain(int k) {
    for (;;) {
        if (!g_par_run || g_ap_start[k] != 1) { par_idle_hlt(); continue; }
        vCPU* c = vcpus[k];
        if (!c || !ept_root) { g_ap_start[k] = 0; continue; }
        // 1. vmx root + the vmcs on this cpu
        if (!VMM::SetupVCPU(c)) { SerialLogger::Log("[par] vcpu setup failed\r\n"); g_ap_start[k] = 0; continue; }
        VMM::VMWrite(VMCS_EPT_POINTER, EPTManager::BuildEPTP(ept_root));
        if (io_bitmap_a) VMM::VMWrite(VMCS_IO_BITMAP_A, KernelPhysOf(io_bitmap_a));
        if (io_bitmap_b) VMM::VMWrite(VMCS_IO_BITMAP_B, KernelPhysOf(io_bitmap_b));
        if (msr_bitmap) VMM::VMWrite(VMCS_MSR_BITMAP_ADDR, KernelPhysOf(msr_bitmap));
        {   // same cr0 / cr4 ownership as the bsp
            uint64_t f0 = 0, f1 = ~0ULL;
            VMM::SafeReadMSR(MSR_IA32_VMX_CR0_FIXED0, f0);
            VMM::VMWrite(VMCS_CR0_GUEST_HOST_MASK, f0 & ~((1ULL << 0) | (1ULL << 31)));
            VMM::SafeReadMSR(MSR_IA32_VMX_CR4_FIXED0, f0);
            VMM::SafeReadMSR(MSR_IA32_VMX_CR4_FIXED1, f1);
            VMM::VMWrite(VMCS_GUEST_CR4, f0 & f1);
            VMM::VMWrite(VMCS_CR4_GUEST_HOST_MASK, f0 | ~f1);
            VMM::VMWrite(VMCS_CR4_READ_SHADOW, 0);
        }
        // preemption timer (~100 us) and the tsc offset
        {
            uint64_t basic = VMM::ReadMSR(0x480);
            uint64_t pin = VMM::ReadMSR(((basic >> 55) & 1) ? 0x48D : 0x481);
            uint32_t rate = (uint32_t)(VMM::ReadMSR(0x485) & 0x1F);
            uint64_t tsc = CPUDetect::GetInfo().frequency.tsc_frequency;
            if (tsc < 1000000ULL) tsc = 2000000000ULL;
            uint64_t ticks = (tsc / 10000) >> rate; if (ticks < 1) ticks = 1;
            if ((pin >> 32) & PIN_PREEMPT_TIMER) {
                VMM::VMWrite(VMCS_PIN_BASED_CONTROLS, VMM::VMRead(VMCS_PIN_BASED_CONTROLS) | PIN_PREEMPT_TIMER);
                VMM::VMWrite(0x482E, ticks);
            }
        }
        int64_t off = par_tsc_offset();
        VMExitHandler::SetTscOffset(SMP::CpuIndex(), off);
        VMM::VMWrite(VMCS_PROC_BASED_CONTROLS, VMM::VMRead(VMCS_PROC_BASED_CONTROLS) | PROC_USE_TSC_OFFSET);
        VMM::VMWrite(0x2010, (uint64_t)off);
        // 2. real mode start at the sipi page
        VMM::SetupVMCSGuestState(c);
        {
            uint64_t f0 = 0, f1 = ~0ULL;
            VMM::SafeReadMSR(MSR_IA32_VMX_CR4_FIXED0, f0);
            VMM::SafeReadMSR(MSR_IA32_VMX_CR4_FIXED1, f1);
            VMM::VMWrite(VMCS_GUEST_CR4, f0 & f1);
        }
        uint64_t seg = (uint64_t)g_ap_vec[k] << 8;
        VMM::VMWrite(VMCS_GUEST_CS_SEL, seg);
        VMM::VMWrite(VMCS_GUEST_CS_BASE, seg << 4);
        VMM::VMWrite(VMCS_GUEST_RIP, 0);
        for (int i = 0; i < 16; i++) c->regs[i] = 0;
        for (int i = 0; i < 5; i++) c->sw_msr[i] = 0;
        for (int i = 0; i < 8; i++) c->dr[i] = 0;
        c->dr[6] = 0xFFFF0FF0ULL; c->dr[7] = 0x400ULL;
        for (int i = 0; i < 512; i++) c->fx[i] = g_vmx_guest_fx[i];
        c->launched = false;
        uint32_t seen_gen = g_ept_gen;
        { uint64_t d[2] = { 0, 0 }; (void)d; }
        g_ap_start[k] = 2; ap_state[k] = 1; vhalted[k] = false;
        SerialLogger::Log("[par] vcpu "); SerialLogger::LogDec(k); SerialLogger::Log(" runs on cpu ");
        SerialLogger::LogDec((int)SMP::CpuIndex()); SerialLogger::Log(" tsc offset "); SerialLogger::LogDec((int)(off / 1000)); SerialLogger::Log(" kcycles\r\n");
        // 3. run
        while (g_par_run && g_ap_start[k] == 2) {
            vlock(); ParSetCtx(k);
            VLapic::Poll(VMExitHandler::GuestClockNs());
            if (vhalted[k]) {
                if (VLapic::Pending(k) < 0) { vunlock(); par_idle_hlt(); continue; }
                vhalted[k] = false;
            }
            CheckAndInjectPendingIRQs();
            vunlock();
            { uint32_t g = g_ept_gen; if (g != seen_gen || g != g_seen_gen[k]) { seen_gen = g; EPTManager::InvalidateEPT(); g_seen_gen[k] = g; } }
            __atomic_store_n(&g_in_guest[k], (uint8_t)1, __ATOMIC_SEQ_CST);
            int er = VMM::RunVCPU(c);
            __atomic_store_n(&g_in_guest[k], (uint8_t)0, __ATOMIC_SEQ_CST);
            if (er < 0) { vlock(); if (vm_state == VM_STATE_RUNNING) vm_state = VM_STATE_CRASHED; vunlock(); break; }
            bool kls = Scheduler::SchedV2() && par_exit_needs_kls(c);
            if (kls) LinuxSyscall::LayerLock();
            vlock(); ParSetCtx(k);
            VMState st = ProcessVMExit();
            if (reboot_requested) { reboot_requested = false; st = VM_STATE_REBOOTING; }
            if (st != VM_STATE_RUNNING && vm_state == VM_STATE_RUNNING) vm_state = st;
            vunlock();
            if (kls) LinuxSyscall::LayerUnlock();
            if (vm_state != VM_STATE_RUNNING && vm_state != VM_STATE_PAUSED) break;
        }
        // 4. leave vmx state of this vcpu on this cpu
        VMM::VMClear(c->vmcs);
        ap_state[k] = 0; vhalted[k] = false;
        g_ap_start[k] = 0;
    }
}

template<int K> static void ap_tramp() { Hypervisor::ApMain(K); }

void Hypervisor::ParStart() {
    g_par_on = false;
    if (nvcpus < 2 || !VMHost::Par()) return;
    for (int k = 1; k < nvcpus; k++) if (!Scheduler::VcpuCpu(k)) return;      // not planned: time-multiplexed instead
    static void (*const tr[8])() = { nullptr, ap_tramp<1>, ap_tramp<2>, ap_tramp<3>, ap_tramp<4>, ap_tramp<5>, ap_tramp<6>, ap_tramp<7> };
    static const char* nm[8] = { "", "vmcpu1", "vmcpu2", "vmcpu3", "vmcpu4", "vmcpu5", "vmcpu6", "vmcpu7" };
    for (int k = 1; k < nvcpus; k++) {
        g_vcpu_cpu[k] = Scheduler::VcpuCpu(k);
        g_ap_start[k] = 0;
        if (!g_ap_spawned[k]) { g_ap_spawned[k] = true; Scheduler::SpawnKernelProcess(nm[k], tr[k], PRIO_NORMAL, 64, 256); }
    }
    g_bsp_cpu = SMP::CpuIndex();
    g_vlock = 0; g_sync_req = 0; g_sync_owner = 0;
    g_par_on = true; g_par_run = true;
    VLapic::SetKick(KickVcpu);
    SerialLogger::Log("[par] parallel vcpus on\r\n");
}

void Hypervisor::ParStop() {
    if (!g_par_on) return;
    g_par_run = false;
    for (int k = 1; k < nvcpus; k++) KickVcpu(k);
    uint64_t t0 = Timer::GetRealMs64();
    for (;;) {
        bool any = false;
        for (int k = 1; k < nvcpus; k++) if (g_ap_start[k] != 0) any = true;
        if (!any || Timer::GetRealMs64() - t0 > 1500) break;
        for (int k = 1; k < nvcpus; k++) KickVcpu(k);
        Scheduler::SleepMs(1);
    }
    VLapic::SetKick(nullptr);
    g_par_on = false;
}

VMState Hypervisor::RunSlicePar(uint32_t budget_us, bool* idle) {
    uint64_t t0 = VMExitHandler::GuestClockNs() / 1000;
    for (;;) {
        par_serve_sync();
        vlock();
        uint64_t nowns = VMExitHandler::GuestClockNs();
        uint64_t now = nowns / 1000;
        if (now - g_slice_last_us >= 250) {
            uint64_t d = now - g_slice_last_us;
            if (d > 100000) d = 100000;
            g_slice_last_us = now;
            stats.tick_count++;
            VirtualDevices::Tick((uint32_t)d);
            if (config.enable_serial) serial.Tick((uint32_t)d);
            GuestConsole::Poll(0); GuestConsole::Poll(1); GuestConsole::Poll(2); GuestConsole::Poll(3);
            VNet::Poll();
            VirtioPollAll();
        }
        VLapic::Poll(nowns);
        ParSetCtx(0);
        if (vm_state != VM_STATE_RUNNING && vm_state != VM_STATE_PAUSED) { VMState st = vm_state; vunlock(); return st; }
        if (vhalted[0]) {
            bool pend = VLapic::Pending(0) >= 0 || GuestIrqPending();
            if (!pend) {
                bool others = false;
                for (int k = 1; k < nvcpus; k++) if (g_ap_start[k] == 2 && !vhalted[k]) others = true;
                bool off = vcpu->type == VIRT_INTEL_VTX && !(VMM::VMRead(VMCS_GUEST_RFLAGS) & (1u << 9));
                vunlock();
                if (!others && off) { vm_state = VM_STATE_HALTED; return vm_state; }
                if (!others && idle) *idle = true;
                return vm_state;
            }
            vhalted[0] = false;
        }
        CheckAndInjectPendingIRQs();
        vunlock();
        { uint32_t g = g_ept_gen; if (g != g_seen_gen[0]) { EPTManager::InvalidateEPT(); g_seen_gen[0] = g; } }
        int kls_depth = Scheduler::SchedV2() ? LinuxSyscall::KlsDropAll() : 0;
        __atomic_store_n(&g_in_guest[0], (uint8_t)1, __ATOMIC_SEQ_CST);
        int er = VMM::RunVCPU(vcpus[0]);
        __atomic_store_n(&g_in_guest[0], (uint8_t)0, __ATOMIC_SEQ_CST);
        LinuxSyscall::KlsRestore(kls_depth);
        if (er < 0) return VM_STATE_CRASHED;
        vlock(); ParSetCtx(0);
        VMState st = ProcessVMExit();
        if (reboot_requested) { reboot_requested = false; st = VM_STATE_REBOOTING; }
        vunlock();
        if (st != VM_STATE_RUNNING) return st;
        if (VMExitHandler::GuestClockNs() / 1000 - t0 >= budget_us) break;
    }
    return vm_state;
}

// smp: the vcpus take turns on this thread, one vm exit each; a vcpu that halted sleeps until
// its lapic (or, for the bsp, the pic) has something for it (satoru)
VMState Hypervisor::RunSliceSmp(uint32_t budget_us, bool* idle) {
    uint64_t t0 = VMExitHandler::GuestClockNs() / 1000;
    for (;;) {
        uint64_t nowns = VMExitHandler::GuestClockNs();
        uint64_t now = nowns / 1000;
        if (now - g_slice_last_us >= 250) {
            uint64_t d = now - g_slice_last_us;
            if (d > 100000) d = 100000;
            g_slice_last_us = now;
            stats.tick_count++;
            VirtualDevices::Tick((uint32_t)d);
            if (config.enable_serial) serial.Tick((uint32_t)d);
            GuestConsole::Poll(0); GuestConsole::Poll(1); GuestConsole::Poll(2); GuestConsole::Poll(3);
            VNet::Poll();
            VirtioPollAll();
        }
        VLapic::Poll(nowns);
                bool ran = false;
        for (int k = 0; k < nvcpus; k++) {
            if (!ap_state[k]) continue;
            SwitchVcpu(k);
            if (vhalted[k]) {
                bool pend = VLapic::Pending(k) >= 0 || (k == 0 && GuestIrqPending());
                if (!pend) continue;
                vhalted[k] = false;
            }
            guest_halted = false;
            // a turn lasts until the vcpu halts, spins in a pause loop, or ~250 us have passed;
            // one exit per turn starved the bsp behind its own timer interrupts (satoru)
            uint64_t qs = VMExitHandler::GuestClockNs();
            for (;;) {
                yield_req = false;
                vm_state = RunOneCycle();
                ran = true;
                if (reboot_requested) { reboot_requested = false; vm_state = VM_STATE_REBOOTING; }
                if (vm_state != VM_STATE_RUNNING) return vm_state;
                if (vhalted[k] || yield_req) break;
                uint64_t t = VMExitHandler::GuestClockNs();
                if (t - qs >= 250000) break;
                VLapic::Poll(t);
            }
        }
        if (!ran) {
            // every started vcpu is halted with nothing pending. the bsp halting with interrupts
            // off is the guest's poweroff (satoru)
            SwitchVcpu(0);
            if (vcpu->type == VIRT_INTEL_VTX && vhalted[0] && !(VMM::VMRead(VMCS_GUEST_RFLAGS) & (1u << 9))) {
                vm_state = VM_STATE_HALTED;
                return vm_state;
            }
            if (idle) *idle = true;
            return vm_state;
        }
        if (VMExitHandler::GuestClockNs() / 1000 - t0 >= budget_us) break;
    }
    return vm_state;
}

bool Hypervisor::BootDebianWithExtraction(uint32_t max_boot_exits) {
    (void)max_boot_exits;
    if (IsOwned()) return false;    // the background vm host has it (satoru)
    if (!DebianPrepare()) return false;
    // synchronous variant: run for a while and capture the console (satoru)
    uint64_t t0 = Timer::GetRealMs64();
    while (vm_state == VM_STATE_RUNNING && Timer::GetRealMs64() - t0 < 20000) {
        bool idle = false;
        RunSlice(4000, &idle);
        PumpGuestConsole(serial, debian_boot_log, debian_boot_log_len, (int)sizeof(debian_boot_log));
        KuronoShell::PumpUI();
    }
    debian_booted = vm_state == VM_STATE_RUNNING;
    return debian_booted;
}

// bootalpinesoftware - removed
// all stubs removed. alpine requires real hardware virtualization (amd-v / vt-x).
// honest failure > fake success.

// softwarealpineexec - removed
// no fake command responses. alpine runs real or not at all.

//  extractalpinedrivers - parse boot log + query /proc /sys for drivers
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
            drv.version[3] = 0; // "alp" - alpine-sourced
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
            // we just log this for now - the boot log parse above covers most
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

//  runalpinecycles - incremental alpine vm execution

// rolling tail of guest serial text, so the boot loop can stop at a shell
// prompt or a panic instead of at the first byte of output (satoru)

static bool TailHas(const char* s) {
    int n = 0;
    while (s[n]) n++;
    for (int i = 0; i + n <= g_guest_tail_len; i++) {
        if (memcmp(g_guest_tail + i, s, n) == 0) return true;
    }
    return false;
}

static void NoteGuestText(const char* t, int n) {
    for (int i = 0; i < n; i++) {
        if (g_guest_tail_len == (int)sizeof(g_guest_tail)) {
            memmove(g_guest_tail, g_guest_tail + 64, sizeof(g_guest_tail) - 64);
            g_guest_tail_len -= 64;
        }
        g_guest_tail[g_guest_tail_len++] = t[i];
    }
    if (TailHas("login:") || TailHas(" # ") || TailHas("/ #") || TailHas("~ #"))
        g_guest_prompt_seen = true;
    if (TailHas("Kernel panic") || TailHas("end Kernel panic"))
        g_guest_panic_seen = true;
}

VMState Hypervisor::RunAlpineCycles(uint32_t max_exits) {
    if (ForeignCaller()) return vm_state;   // (satoru)

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
                NoteGuestText(alpine_boot_log + alpine_boot_log_len, n); // (satoru)
                alpine_boot_log_len += n;
                alpine_boot_log[alpine_boot_log_len] = 0;
            }
        } else {
            // boot log full - just drain and discard
            char discard[256];
            int n;
            while ((n = serial.ReadOutput(discard, (int)sizeof(discard) - 1)) > 0)
                NoteGuestText(discard, n); // (satoru)
        }
    }

    return vm_state;
}

// mirror the guest console to the host serial log, one "[g] " line at a time,
// and keep the tail in the boot log buffer for the shell commands. (satoru)
static void PumpGuestConsole(VirtualSerial& ser, char* keep, int& keep_len, int keep_max) {
    static char line[256]; static int ll = 0;
    char buf[256];
    while (ser.HasOutput()) {
        int n = ser.ReadOutput(buf, (int)sizeof(buf) - 1);
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (keep_len < keep_max - 1) { keep[keep_len++] = c; keep[keep_len] = 0; }
            if (c == '\r') continue;
            if (c == '\n' || ll >= (int)sizeof(line) - 2) {
                line[ll] = 0;
                SerialLogger::Log("[g] "); SerialLogger::Log(line); SerialLogger::Log("\r\n");
                ll = 0;
                if (c != '\n') line[ll++] = c;
            } else line[ll++] = c;
        }
    }
}

VMState Hypervisor::RunDebianCycles(uint32_t max_exits) {
    if (ForeignCaller()) return vm_state;   // (satoru)

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

    PumpGuestConsole(serial, debian_boot_log, debian_boot_log_len, (int)sizeof(debian_boot_log));

    return vm_state;
}

//  alpineexec - send command to guest, run cycles, capture output

int Hypervisor::AlpineExec(const char* cmd, char* out_buf, int out_max) {
    // the background vm: the command goes through its guest agent. never from
    // the vm thread itself, which would wait on its own output (satoru)
    if (IsOwned()) {
        if (!ForeignCaller() || !cmd || !out_buf || out_max <= 0) return 0;
        int n = VMHost::Exec(cmd, out_buf, out_max + 1, 600000);
        return n < 0 ? 0 : n;
    }
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
            if (i % 128 == 0) KuronoShell::PumpUI();
        }

        // read whatever the guest produced
        if (serial.HasOutput()) {
            int n = serial.ReadOutput(out_buf + total_read, out_max - total_read - 1);
            if (n > 0) total_read += n;
        }
        KuronoShell::PumpUI();

        // if we got some output and guest likely finished (no more coming for a cycle)
        if (total_read > 0 && !serial.HasOutput()) {
            // run a few more cycles to see if more output comes
            for (uint32_t i = 0; i < 1000 && vm_state == VM_STATE_RUNNING; i++) {
                vm_state = RunOneCycle();
                if ((i & 127u) == 0) KuronoShell::PumpUI();
            }
            if (!serial.HasOutput()) break; // cmd finished
            // still output coming - read more
            int n = serial.ReadOutput(out_buf + total_read, out_max - total_read - 1);
            if (n > 0) total_read += n;
        }

        if (vm_state != VM_STATE_RUNNING) break;
    }

    if (total_read > 0) out_buf[total_read] = 0;
    return total_read;
}

int Hypervisor::DebianExec(const char* cmd, char* out_buf, int out_max) {
    if (IsOwned()) return 0;   // the background vm host has the vm (satoru)
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
            if (i % 128 == 0) KuronoShell::PumpUI();
        }

        if (serial.HasOutput()) {
            int n = serial.ReadOutput(out_buf + total_read, out_max - total_read - 1);
            if (n > 0) total_read += n;
        }
        KuronoShell::PumpUI();

        if (total_read > 0 && !serial.HasOutput()) {
            for (uint32_t i = 0; i < 1000 && vm_state == VM_STATE_RUNNING; i++) {
                vm_state = RunOneCycle();
                if ((i & 127u) == 0) KuronoShell::PumpUI();
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

// the background alpine vm counts as booted once its agent answers (satoru)
bool Hypervisor::IsAlpineBooted() {
    if (IsOwned()) return VMHost::AgentReady() && VMHost::DistroName()[0] == 'a';
    return alpine_booted;
}
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
