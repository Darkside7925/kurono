// ═══════════════════════════════════════════════════════════════════════════
//  Kurono OS — Hypervisor / VM Lifecycle Manager Implementation
//  Orchestrates all virtualization components to create and run a VM
//  capable of booting a Linux kernel.
//
//  Architecture:
//    Hypervisor::Init()
//      → Detect hardware (VT-x / AMD-V)
//      → Initialize VMM
//    Hypervisor::CreateVM(config)
//      → Allocate guest RAM (GuestMemoryManager)
//      → Create vCPU, EPT/NPT
//      → Initialize virtual devices (PIC, APIC, PIT, HPET, Serial, Disk)
//      → Set up I/O bitmap, MSR bitmap
//    Hypervisor::LoadLinuxKernel(bzimage, cmdline)
//      → Parse bzImage header (LinuxBootLoader)
//      → Load setup code + kernel into guest memory
//      → Fill boot_params, E820, command line
//    Hypervisor::RunVM()
//      → Configure VMCS/VMCB for protected-mode entry
//      → VM-entry → VM-exit → Handle → re-enter loop
//      → Tick virtual devices periodically
//      → Inject interrupts when guest is interruptible
//
//  Reference: Intel SDM Vol 3C Chapters 23-33, AMD APM Vol 2 Chapter 15
// ═══════════════════════════════════════════════════════════════════════════
#include "hypervisor.h"
#include "../kernel/types.h"
#include "../kernel/heap.h"
#include "../drivers/serial.h"

// Helper: allocate aligned memory from kernel heap
static void* HVAllocAligned(size_t size, size_t align) {
    void* raw = KernelHeap::Alloc(size + align);
    if (!raw) return nullptr;
    uintptr_t addr = (uintptr_t)raw;
    uintptr_t aligned = (addr + align - 1) & ~(align - 1);
    return (void*)aligned;
}

// ── Static members ──
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

// ═══════════════════════════════════════════════════════════════════════════
//  Init — detect hardware virtualization
// ═══════════════════════════════════════════════════════════════════════════

bool Hypervisor::Init() {
    SerialLogger::Log("Hypervisor: Initializing...\r\n");

    // Initialize the VMM (hardware detection)
    VMM::Init();
    hw_available = VMM::IsSupported();

    if (hw_available) {
        SerialLogger::Log("Hypervisor: Hardware virtualization available (");
        SerialLogger::Log(VMM::GetType() == VIRT_INTEL_VTX ? "Intel VT-x" : "AMD-V");
        SerialLogger::Log(")\r\n");
    } else {
        SerialLogger::Log("Hypervisor: No hardware virtualization — "
                         "software emulation only\r\n");
    }

    // Initialize exit handler
    VMExitHandler::Init();

    // Initialize EPT manager
    EPTManager::Init();

    // Initialize virtual devices
    VirtualDevices::Init();

    ResetStats();
    vm_state = VM_STATE_UNINITIALIZED;

    SerialLogger::Log("Hypervisor: Init complete\r\n");
    return true;
}

bool Hypervisor::IsAvailable() {
    return hw_available;
}

// ═══════════════════════════════════════════════════════════════════════════
//  CreateVM — set up a new virtual machine
// ═══════════════════════════════════════════════════════════════════════════

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

    // ── Step 1: Allocate guest RAM ───────────────────────────────────────
    GuestMemoryManager::Init(config.ram_mb);
    if (!GuestMemoryManager::GetLowRAM()) {
        SerialLogger::Log("Hypervisor: Failed to allocate guest memory\r\n");
        return false;
    }

    // ── Step 2: Create vCPU ──────────────────────────────────────────────
    if (hw_available) {
        vcpu = VMM::CreateVCPU();
        if (!vcpu) {
            SerialLogger::Log("Hypervisor: Failed to create vCPU\r\n");
            GuestMemoryManager::FreeGuestRAM();
            return false;
        }
    } else {
        // Software emulation: create a dummy vCPU
        vcpu = (vCPU*)KernelHeap::Alloc(sizeof(vCPU));
        if (vcpu) {
            memset(vcpu, 0, sizeof(vCPU));
            vcpu->type = VIRT_NONE;
        }
    }

    // ── Step 3: Set up EPT/NPT ──────────────────────────────────────────
    if (hw_available) {
        if (!SetupEPT()) {
            SerialLogger::Log("Hypervisor: EPT setup failed\r\n");
        }
    }

    // ── Step 4: Set up I/O and MSR bitmaps ───────────────────────────────
    if (hw_available) {
        SetupIOBitmap();
        SetupMSRBitmap();
    }

    // ── Step 5: Initialize virtual devices ───────────────────────────────
    SetupDevices();

    vm_state = VM_STATE_CREATED;
    SerialLogger::Log("Hypervisor: VM created successfully\r\n");
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
//  DestroyVM — tear down and free all VM resources
// ═══════════════════════════════════════════════════════════════════════════

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

    if (io_bitmap_a)  { KernelHeap::Free(io_bitmap_a); io_bitmap_a = nullptr; }
    if (io_bitmap_b)  { KernelHeap::Free(io_bitmap_b); io_bitmap_b = nullptr; }
    if (msr_bitmap)   { KernelHeap::Free(msr_bitmap); msr_bitmap = nullptr; }

    GuestMemoryManager::FreeGuestRAM();

    vm_state = VM_STATE_DESTROYED;
    SerialLogger::Log("Hypervisor: VM destroyed\r\n");
}

// ═══════════════════════════════════════════════════════════════════════════
//  SetupIOBitmap — configure which I/O ports cause VM-exits
// ═══════════════════════════════════════════════════════════════════════════

bool Hypervisor::SetupIOBitmap() {
    // Allocate two 4 KB pages (must be page-aligned)
    io_bitmap_a = (uint8_t*)HVAllocAligned(IO_BITMAP_A_SIZE, 4096);
    io_bitmap_b = (uint8_t*)HVAllocAligned(IO_BITMAP_B_SIZE, 4096);
    if (!io_bitmap_a || !io_bitmap_b) {
        SerialLogger::Log("Hypervisor: Failed to alloc IO bitmaps\r\n");
        return false;
    }

    // Set all bits = 1 → all I/O ports cause VM-exit
    // This gives us full control but we can selectively allow later
    memset(io_bitmap_a, 0xFF, IO_BITMAP_A_SIZE);
    memset(io_bitmap_b, 0xFF, IO_BITMAP_B_SIZE);

    // If we want to pass through certain ports, clear their bits:
    // For example, pass through debug port 0xE9 (QEMU debug console)
    // Port 0xE9 is in bitmap A (port < 0x8000)
    // Byte index = port / 8, bit = port % 8
    // io_bitmap_a[0xE9 / 8] &= ~(1 << (0xE9 % 8));

    // Write bitmap addresses to VMCS if Intel VMX
    if (VMM::GetType() == VIRT_INTEL_VTX && vcpu && vcpu->vmcs) {
        VMM::VMPtrLoad(vcpu->vmcs);
        VMM::VMWrite(VMCS_IO_BITMAP_A, (uint32_t)(uintptr_t)io_bitmap_a);
        VMM::VMWrite(VMCS_IO_BITMAP_B, (uint32_t)(uintptr_t)io_bitmap_b);
    }

    SerialLogger::Log("Hypervisor: I/O bitmap configured (intercept all)\r\n");
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
//  SetupMSRBitmap — configure which MSRs cause VM-exits
// ═══════════════════════════════════════════════════════════════════════════

bool Hypervisor::SetupMSRBitmap() {
    msr_bitmap = (uint8_t*)HVAllocAligned(MSR_BITMAP_SIZE, 4096);
    if (!msr_bitmap) {
        SerialLogger::Log("Hypervisor: Failed to alloc MSR bitmap\r\n");
        return false;
    }

    // Layout of MSR bitmap (4096 bytes):
    //   Bytes 0x000-0x3FF: Read bitmap for MSRs 0x00000000-0x00001FFF
    //   Bytes 0x400-0x7FF: Read bitmap for MSRs 0xC0000000-0xC0001FFF
    //   Bytes 0x800-0xBFF: Write bitmap for MSRs 0x00000000-0x00001FFF
    //   Bytes 0xC00-0xFFF: Write bitmap for MSRs 0xC0000000-0xC0001FFF

    // Start with all MSRs intercepted
    memset(msr_bitmap, 0xFF, MSR_BITMAP_SIZE);

    // Allow safe MSRs to pass through (no VM-exit):
    // IA32_TIME_STAMP_COUNTER (0x10) — let guest read TSC directly
    uint32_t tsc_byte = 0x10 / 8;
    uint32_t tsc_bit  = 0x10 % 8;
    msr_bitmap[tsc_byte] &= ~(1 << tsc_bit);               // Read pass-through
    msr_bitmap[0x800 + tsc_byte] &= ~(1 << tsc_bit);       // Write pass-through

    // Write MSR bitmap address to VMCS
    if (VMM::GetType() == VIRT_INTEL_VTX && vcpu && vcpu->vmcs) {
        VMM::VMPtrLoad(vcpu->vmcs);
        VMM::VMWrite(VMCS_MSR_BITMAP_ADDR, (uint32_t)(uintptr_t)msr_bitmap);
    }

    SerialLogger::Log("Hypervisor: MSR bitmap configured\r\n");
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
//  SetupEPT — create extended page tables mapping guest physical memory
// ═══════════════════════════════════════════════════════════════════════════

bool Hypervisor::SetupEPT() {
    if (VMM::GetType() == VIRT_INTEL_VTX) {
        ept_root = EPTManager::CreateEPT();
        if (!ept_root) return false;

        // Map low RAM (0 - 640 KB) = identity map to host allocation
        uint8_t* low_ram = GuestMemoryManager::GetLowRAM();
        uint32_t low_size = GuestMemoryManager::GetLowRAMSize();
        if (low_ram && low_size > 0) {
            EPTManager::MapRAM(ept_root, 0x00000000ULL,
                               (uint64_t)(uintptr_t)low_ram, low_size);
            EPTManager::AddRegion({0x00000000ULL,
                                   (uint64_t)(uintptr_t)low_ram,
                                   low_size, MEM_RAM, true, true, true});
        }

        // Map VGA buffer (0xA0000 - 0xBFFFF)
        uint8_t* vga = GuestMemoryManager::GetVGABuffer();
        if (vga) {
            EPTManager::MapMMIO(ept_root, GUEST_VGA_BASE,
                                (uint64_t)(uintptr_t)vga, (uint32_t)GUEST_VGA_SIZE);
        }

        // Map ROM area (0xC0000 - 0xFFFFF)
        uint8_t* rom = GuestMemoryManager::GetROMArea();
        if (rom) {
            EPTManager::MapROM(ept_root, GUEST_ROM_BASE,
                               (uint64_t)(uintptr_t)rom, (uint32_t)GUEST_ROM_SIZE);
        }

        // Map high RAM (1 MB+)
        uint8_t* high_ram = GuestMemoryManager::GetHighRAM();
        uint32_t high_size = GuestMemoryManager::GetHighRAMSize();
        if (high_ram && high_size > 0) {
            EPTManager::MapRAM(ept_root, GUEST_HIGH_RAM_START,
                               (uint64_t)(uintptr_t)high_ram, high_size);
            EPTManager::AddRegion({GUEST_HIGH_RAM_START,
                                   (uint64_t)(uintptr_t)high_ram,
                                   high_size, MEM_RAM, true, true, true});
        }

        // Map LAPIC MMIO (0xFEE00000)
        EPTManager::AddRegion({GUEST_LAPIC_BASE, 0, 0x1000, MEM_MMIO,
                               true, true, false});

        // Map HPET MMIO (0xFED00000)
        EPTManager::AddRegion({GUEST_HPET_BASE, 0, 0x400, MEM_MMIO,
                               true, true, false});

        // Write EPTP to VMCS
        uint64_t eptp = EPTManager::BuildEPTP(ept_root);
        if (vcpu && vcpu->vmcs) {
            VMM::VMPtrLoad(vcpu->vmcs);
            VMM::VMWrite(VMCS_EPT_POINTER, (uint32_t)(eptp & 0xFFFFFFFF));
            VMM::VMWrite(VMCS_EPT_POINTER + 1, (uint32_t)(eptp >> 32));
        }

        SerialLogger::Log("Hypervisor: EPT configured\r\n");

    } else if (VMM::GetType() == VIRT_AMD_SVM) {
        npt_root = EPTManager::CreateNPT();
        if (!npt_root) return false;

        // Same mappings but using NPT
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

        // Set nCR3 in VMCB
        if (vcpu && vcpu->vmcb) {
            vcpu->vmcb->n_cr3 = EPTManager::BuildNCR3(npt_root);
            vcpu->vmcb->np_enable = 1;
        }

        SerialLogger::Log("Hypervisor: NPT configured\r\n");
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
//  SetupDevices — initialize all virtual devices
// ═══════════════════════════════════════════════════════════════════════════

bool Hypervisor::SetupDevices() {
    // Serial port (COM1)
    if (config.enable_serial) {
        serial.Init(COM1_BASE, COM1_IRQ);
        SerialLogger::Log("Hypervisor: Virtual COM1 enabled\r\n");
    }

    // IDE disk
    if (config.enable_disk && config.disk_size_mb > 0) {
        disk.Init(config.disk_size_mb * 1024 * 1024);
        SerialLogger::Log("Hypervisor: Virtual IDE disk enabled\r\n");
    }

    // PIC/APIC/PIT/HPET already initialized by VirtualDevices::Init()

    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
//  LoadLinuxKernel — load a bzImage into guest memory
// ═══════════════════════════════════════════════════════════════════════════

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

// ═══════════════════════════════════════════════════════════════════════════
//  ConfigureGuestProtectedMode — set up VMCS/VMCB for 32-bit protected mode
//  This is how Linux expects to be entered (per boot protocol ≥ 2.00):
//    - Protected mode enabled (CR0.PE = 1)
//    - Flat 4 GB code/data segments
//    - Paging disabled (CR0.PG = 0)
//    - A20 gate enabled
//    - GDT loaded with flat segments
//    - CS = __BOOT_CS (selector with flat 4 GB code segment)
//    - DS = ES = FS = GS = SS = __BOOT_DS (flat 4 GB data segment)
//    - EIP = code32_start (normally 0x100000)
//    - ESI = boot_params address (pointer to struct boot_params)
//    - Interrupts disabled (EFLAGS.IF = 0)
// ═══════════════════════════════════════════════════════════════════════════

void Hypervisor::ConfigureGuestProtectedMode(uint32_t entry_point,
                                              uint32_t boot_params_addr) {
    if (!vcpu) return;

    if (vcpu->type == VIRT_INTEL_VTX) {
        VMM::VMPtrLoad(vcpu->vmcs);

        // ── CR0: Protected mode ON, paging OFF ──────────────────────────
        // CR0.PE = 1, CR0.NE = 1, CR0.ET = 1
        uint32_t cr0 = 0x00000031; // PE + ET + NE
        VMM::VMWrite(VMCS_GUEST_CR0, cr0);
        VMM::VMWrite(VMCS_GUEST_CR3, 0);
        VMM::VMWrite(VMCS_GUEST_CR4, 0);

        // ── Segment Registers: Flat 4 GB ────────────────────────────────
        // CS: selector 0x10, flat code segment, 32-bit, DPL 0
        VMM::VMWrite(VMCS_GUEST_CS_SEL, 0x0010);
        VMM::VMWrite(VMCS_GUEST_CS_BASE, 0x00000000);
        VMM::VMWrite(VMCS_GUEST_CS_LIMIT, 0xFFFFFFFF);
        VMM::VMWrite(VMCS_GUEST_CS_AR, 0xC09B); // G=1,D=1,P=1,DPL=0,Code,R/X

        // DS/ES/FS/GS/SS: selector 0x18, flat data segment, 32-bit, DPL 0
        uint32_t data_ar = 0xC093; // G=1,B=1,P=1,DPL=0,Data,R/W
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

        // FS and GS set to null to match Linux entry expectations
        VMM::VMWrite(VMCS_GUEST_FS_SEL, 0x0000);
        VMM::VMWrite(VMCS_GUEST_FS_BASE, 0x00000000);
        VMM::VMWrite(VMCS_GUEST_FS_LIMIT, 0x0000FFFF);
        VMM::VMWrite(VMCS_GUEST_FS_AR, 0x0093); // 16-bit data

        VMM::VMWrite(VMCS_GUEST_GS_SEL, 0x0000);
        VMM::VMWrite(VMCS_GUEST_GS_BASE, 0x00000000);
        VMM::VMWrite(VMCS_GUEST_GS_LIMIT, 0x0000FFFF);
        VMM::VMWrite(VMCS_GUEST_GS_AR, 0x0093);

        // ── GDT: Build a minimal GDT in guest memory ────────────────────
        // Place a 3-entry GDT at guest physical 0x1000:
        //   Entry 0: Null descriptor
        //   Entry 1 (0x08): unused / TSS placeholder
        //   Entry 2 (0x10): 32-bit code, base=0, limit=4GB, DPL=0
        //   Entry 3 (0x18): 32-bit data, base=0, limit=4GB, DPL=0
        uint64_t gdt[4];
        gdt[0] = 0x0000000000000000ULL; // Null
        gdt[1] = 0x0000000000000000ULL; // Unused
        gdt[2] = 0x00CF9B000000FFFFULL; // Code: base=0,limit=4G,G=1,D=1,Type=B
        gdt[3] = 0x00CF93000000FFFFULL; // Data: base=0,limit=4G,G=1,B=1,Type=3

        GuestMemoryManager::WriteGuestPhys(0x1000, gdt, sizeof(gdt));

        VMM::VMWrite(VMCS_GUEST_GDTR_BASE, 0x00001000);
        VMM::VMWrite(VMCS_GUEST_GDTR_LIMIT, sizeof(gdt) - 1);

        // ── IDT: Empty for now (guest will set up its own) ──────────────
        VMM::VMWrite(VMCS_GUEST_IDTR_BASE, 0x00000000);
        VMM::VMWrite(VMCS_GUEST_IDTR_LIMIT, 0); // No IDT entries

        // ── TR (Task Register) ──────────────────────────────────────────
        // Must have a valid TR even in protected mode without tasks
        VMM::VMWrite(VMCS_GUEST_TR_SEL, 0x0008);
        VMM::VMWrite(VMCS_GUEST_TR_BASE, 0x00000000);
        VMM::VMWrite(VMCS_GUEST_TR_LIMIT, 0x0000FFFF);
        VMM::VMWrite(VMCS_GUEST_TR_AR, 0x008B); // 32-bit busy TSS

        // ── LDTR ─────────────────────────────────────────────────────────
        VMM::VMWrite(VMCS_GUEST_LDTR_SEL, 0x0000);
        VMM::VMWrite(VMCS_GUEST_LDTR_BASE, 0x00000000);
        VMM::VMWrite(VMCS_GUEST_LDTR_LIMIT, 0x0000FFFF);
        VMM::VMWrite(VMCS_GUEST_LDTR_AR, 0x0082); // LDT

        // ── Entry point and registers ────────────────────────────────────
        VMM::VMWrite(VMCS_GUEST_RIP, entry_point);
        VMM::VMWrite(VMCS_GUEST_RSP, 0x00000000); // Linux doesn't use stack at entry
        VMM::VMWrite(VMCS_GUEST_RFLAGS, 0x00000002); // Only reserved bit 1

        // ESI = pointer to boot_params
        // This is passed via general-purpose registers, but VMCS doesn't
        // have direct GPR fields — they're managed via the VM-exit save area.
        // For VMLaunch, we set ESI in the vCPU register array.
        vcpu->regs[6] = boot_params_addr; // ESI

        // ── VMCS Link Pointer (required, set to all-ones for "none") ─────
        VMM::VMWrite(VMCS_VMCS_LINK_PTR, 0xFFFFFFFF);
        VMM::VMWrite(VMCS_VMCS_LINK_PTR + 1, 0xFFFFFFFF);

        // ── Guest activity state: Active ─────────────────────────────────
        VMM::VMWrite(VMCS_GUEST_ACTIVITY, 0); // Active
        VMM::VMWrite(VMCS_GUEST_INTERRUPTIBILITY, 0); // No blocking

        SerialLogger::Log("Hypervisor: VMCS configured for protected-mode entry at 0x");
        SerialLogger::LogHex(entry_point);
        SerialLogger::Log(" ESI=0x");
        SerialLogger::LogHex(boot_params_addr);
        SerialLogger::Log("\r\n");

    } else if (vcpu->type == VIRT_AMD_SVM && vcpu->vmcb) {
        VMCB* vmcb = vcpu->vmcb;

        // CR0: PE + NE + ET
        vmcb->cr0 = 0x00000031;
        vmcb->cr3 = 0;
        vmcb->cr4 = 0;
        vmcb->efer = (1ULL << 12); // SVME

        // Code segment: flat 32-bit
        vmcb->cs.selector = 0x0010;
        vmcb->cs.base     = 0x00000000;
        vmcb->cs.limit    = 0xFFFFFFFF;
        vmcb->cs.attrib   = 0x049B; // G=1,D=1,P=1,Code R/X

        // Data segments: flat 32-bit
        vmcb->ss.selector = 0x0018;
        vmcb->ss.base     = 0x00000000;
        vmcb->ss.limit    = 0xFFFFFFFF;
        vmcb->ss.attrib   = 0x0493;

        vmcb->ds = vmcb->ss;
        vmcb->es = vmcb->ss;
        vmcb->fs.selector = 0;
        vmcb->fs.base = 0;
        vmcb->fs.limit = 0xFFFF;
        vmcb->fs.attrib = 0x0093;
        vmcb->gs = vmcb->fs;

        // GDT (same as Intel)
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

        vcpu->regs[6] = boot_params_addr; // ESI

        SerialLogger::Log("Hypervisor: VMCB configured for protected-mode entry\r\n");
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  RunVM — main execution loop
// ═══════════════════════════════════════════════════════════════════════════

VMState Hypervisor::RunVM(uint32_t max_exits) {
    if (vm_state != VM_STATE_CREATED) {
        SerialLogger::Log("Hypervisor: Cannot run — VM not ready\r\n");
        return vm_state;
    }

    // Configure VMCS/VMCB for protected-mode entry
    uint32_t entry = LinuxBootLoader::GetEntryPoint();
    if (entry == 0) entry = LINUX_KERNEL_ADDR;
    ConfigureGuestProtectedMode(entry, LINUX_BOOT_PARAMS_ADDR);

    // Set up VMCS controls
    if (hw_available && vcpu) {
        if (vcpu->type == VIRT_INTEL_VTX) {
            if (!VMM::SetupVCPU(vcpu)) {
                SerialLogger::Log("Hypervisor: VMCS setup failed\r\n");
                vm_state = VM_STATE_CRASHED;
                return vm_state;
            }
        }
    }

    vm_state = VM_STATE_RUNNING;
    SerialLogger::Log("Hypervisor: Entering VM run loop\r\n");

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

        // Periodic device ticking (every 100 exits)
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

// ═══════════════════════════════════════════════════════════════════════════
//  RunOneCycle — single VM-entry → VM-exit → handle cycle
// ═══════════════════════════════════════════════════════════════════════════

VMState Hypervisor::RunOneCycle() {
    if (!vcpu) return VM_STATE_CRASHED;

    stats.run_cycles++;

    // Check for pending interrupts to inject before re-entry
    CheckAndInjectPendingIRQs();

    if (!hw_available) {
        // No hardware virt — can't actually run guest
        // In production this would be a software interpreter
        SerialLogger::Log("Hypervisor: No HW virt, cannot execute guest\r\n");
        return VM_STATE_HALTED;
    }

    // Enter guest
    int exit_reason = VMM::RunVCPU(vcpu);
    if (exit_reason < 0) {
        SerialLogger::Log("Hypervisor: VM-entry failed\r\n");
        return VM_STATE_CRASHED;
    }

    return ProcessVMExit();
}

// ═══════════════════════════════════════════════════════════════════════════
//  ProcessVMExit — dispatch VM-exit to handlers
// ═══════════════════════════════════════════════════════════════════════════

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

// ═══════════════════════════════════════════════════════════════════════════
//  HandleGuestIO — route guest I/O to virtual devices
// ═══════════════════════════════════════════════════════════════════════════

bool Hypervisor::HandleGuestIO(uint16_t port, bool is_out, uint8_t size,
                                 uint32_t& value) {
    stats.io_exits++;

    // Try virtual serial port (COM1: 0x3F8-0x3FF)
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

    // Try virtual IDE disk (0x1F0-0x1F7, 0x3F6)
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

    // Try PIC/PIT/etc via VirtualDevices
    if (VirtualDevices::HandlePortIO(port, is_out, size, value)) {
        return true;
    }

    // Port 0x80 — POST code (ignore)
    if (port == 0x80) return true;

    // Port 0xCF8/0xCFC — PCI configuration (stub: return 0xFFFFFFFF)
    if (port == 0xCF8 || port == 0xCFC) {
        if (!is_out) value = 0xFFFFFFFF;
        return true;
    }

    // Port 0x92 (System Control Port A — A20 gate)
    if (port == 0x92) {
        if (!is_out) value = 0x02; // A20 enabled
        return true;
    }

    // Port 0x70/0x71 — CMOS/RTC
    if (port == 0x70 || port == 0x71) {
        if (!is_out) value = 0;
        return true;
    }

    // Unhandled
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════
//  HandleGuestMMIO — route MMIO to virtual devices
// ═══════════════════════════════════════════════════════════════════════════

bool Hypervisor::HandleGuestMMIO(uint64_t phys_addr, bool is_write,
                                   uint8_t size, uint32_t& value) {
    stats.mmio_exits++;
    return VirtualDevices::HandleMMIO(phys_addr, is_write, size, value);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Interrupt Injection — inject interrupts/exceptions into guest
// ═══════════════════════════════════════════════════════════════════════════

bool Hypervisor::InjectInterrupt(uint8_t vector) {
    if (!vcpu) return false;

    if (vcpu->type == VIRT_INTEL_VTX) {
        VMM::VMPtrLoad(vcpu->vmcs);

        // Check if guest is interruptible
        uint32_t interruptibility = VMM::VMRead(VMCS_GUEST_INTERRUPTIBILITY);
        uint32_t rflags = VMM::VMRead(VMCS_GUEST_RFLAGS);

        // Guest must have IF=1 and not be in STI/MOV SS shadow
        if (!(rflags & (1 << 9)) || (interruptibility & 0x03)) {
            return false; // Not interruptible
        }

        // Write to VM-Entry Interruption-Information field
        // Bits [7:0] = vector, [10:8] = type (0=ext int), [11] = deliver error code
        // [31] = valid
        uint32_t int_info = vector | (0 << 8) | (1u << 31);
        VMM::VMWrite(VMCS_ENTRY_INT_INFO, int_info);
        stats.irq_injections++;
        return true;

    } else if (vcpu->type == VIRT_AMD_SVM && vcpu->vmcb) {
        // SVM event injection via VMCB EventInj field
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

        uint32_t int_info = vector | (3 << 8) | (1u << 31); // Type 3 = HW exception
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
            // AMD SVM: error code goes in bits [63:32] of event_inject
            event |= ((uint64_t)error_code << 32);
        }
        vcpu->vmcb->event_inject = event;
        return true;
    }

    return false;
}

void Hypervisor::CheckAndInjectPendingIRQs() {
    // Check virtual devices for pending IRQs
    int irq = VirtualDevices::GetPendingIRQ();
    if (irq >= 0) {
        InjectInterrupt((uint8_t)irq);
    }

    // Check serial port IRQ
    if (config.enable_serial && serial.HasPendingIRQ()) {
        VirtualDevices::GetMasterPIC().SetIRQ(serial.GetIRQ(), true);
        VirtualDevices::GetMasterPIC().SetIRQ(serial.GetIRQ(), false);
        serial.ClearIRQ();
    }

    // Check IDE IRQ
    if (config.enable_disk && disk.HasPendingIRQ()) {
        VirtualDevices::GetSlavePIC().SetIRQ(6, true); // IRQ 14 = slave IRQ 6
        VirtualDevices::GetSlavePIC().SetIRQ(6, false);
        disk.ClearIRQ();
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  TickDevices — advance virtual device emulation
// ═══════════════════════════════════════════════════════════════════════════

void Hypervisor::TickDevices() {
    stats.tick_count++;
    uint32_t us = config.timer_tick_us;

    VirtualDevices::Tick(us);

    if (config.enable_serial) {
        serial.Tick(us);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Pause / Resume
// ═══════════════════════════════════════════════════════════════════════════

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

// ═══════════════════════════════════════════════════════════════════════════
//  Serial Console
// ═══════════════════════════════════════════════════════════════════════════

int Hypervisor::ReadSerialOutput(char* buf, int max) {
    return serial.ReadOutput(buf, max);
}

bool Hypervisor::HasSerialOutput() {
    return serial.HasOutput();
}

// ═══════════════════════════════════════════════════════════════════════════
//  Statistics
// ═══════════════════════════════════════════════════════════════════════════

void Hypervisor::ResetStats() {
    memset(&stats, 0, sizeof(stats));
}

// ═══════════════════════════════════════════════════════════════════════════
//  Debug
// ═══════════════════════════════════════════════════════════════════════════

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
