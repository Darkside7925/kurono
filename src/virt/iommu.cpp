// ═══════════════════════════════════════════════════════════════════════════
//  Kurono OS — Intel VT-d / IOMMU Implementation
//  DMA remapping for PCI device passthrough (GPU passthrough to Linux guests)
// ═══════════════════════════════════════════════════════════════════════════
#include "iommu.h"
#include "vmm.h"
#include "../drivers/nvidia_gpu.h"
#include "../drivers/serial.h"
#include "../kernel/heap.h"

// ── Static storage ──
IOMMUType       IOMMU::type = IOMMU_NONE;
IOMMUState      IOMMU::iommu_state = IOMMU_STATE_OFF;
DRHD            IOMMU::drhd_units[MAX_DRHD_UNITS] = {};
int             IOMMU::drhd_count = 0;
DeviceAssignment IOMMU::assigned[MAX_DEVICE_ASSIGNS] = {};
int             IOMMU::assign_count = 0;
IOMMURootEntry*   IOMMU::root_table = nullptr;
IOMMUContextEntry* IOMMU::context_tables = nullptr;

// ── String helpers ──
static int iommu_sa(char* o, int p, int mx, const char* s) {
    while (*s && p < mx - 1) o[p++] = *s++;
    o[p] = 0;
    return p;
}
static int iommu_hex(char* o, int p, int mx, uint64_t v) {
    const char h[] = "0123456789ABCDEF";
    char buf[20]; int len = 0;
    if (v == 0) { buf[len++] = '0'; }
    else { char r[20]; int ri = 0; uint64_t t = v; while (t) { r[ri++] = h[t & 0xF]; t >>= 4; } for (int i = ri-1; i >= 0; i--) buf[len++] = r[i]; }
    buf[len] = 0; return iommu_sa(o, p, mx, buf);
}

// ═══════════════════════════════════════════════════════════════════════════
//  VT-d Register Access (MMIO)
// ═══════════════════════════════════════════════════════════════════════════

uint32_t IOMMU::ReadReg(uint64_t base, uint32_t offset) {
    volatile uint32_t* reg = (volatile uint32_t*)(base + offset);
    return *reg;
}

void IOMMU::WriteReg(uint64_t base, uint32_t offset, uint32_t val) {
    volatile uint32_t* reg = (volatile uint32_t*)(base + offset);
    *reg = val;
}

uint64_t IOMMU::ReadReg64(uint64_t base, uint32_t offset) {
    volatile uint64_t* reg = (volatile uint64_t*)(base + offset);
    return *reg;
}

void IOMMU::WriteReg64(uint64_t base, uint32_t offset, uint64_t val) {
    volatile uint64_t* reg = (volatile uint64_t*)(base + offset);
    *reg = val;
}

bool IOMMU::WaitForStatus(uint64_t base, uint32_t bit, bool set, int timeout_us) {
    for (int i = 0; i < timeout_us; i++) {
        uint32_t sts = ReadReg(base, VTD_GSTS_REG);
        if (set  && (sts & bit)) return true;
        if (!set && !(sts & bit)) return true;
        // Spin delay (~1 µs per iteration at typical clock)
        for (volatile int j = 0; j < 1000; j++) {}
    }
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Detection
// ═══════════════════════════════════════════════════════════════════════════

bool IOMMU::DetectVTd() {
    // Check if running under a hypervisor that exposes VT-d
    // In bare-metal, we'd parse the ACPI DMAR table.
    // For QEMU with -machine q35,kernel-irqchip=split -device intel-iommu:
    //   DMAR table is at a known MMIO address.

    // Quick CPUID check: does the CPU support VT-d?
    // VT-d is signaled by CPUID leaf 0x07, ECX bit (varies by CPU)
    // More reliable: check for ACPI DMAR table

    // Scan for ACPI RSDP/RSDT/XSDT → find DMAR table
    // For now, try probing a known QEMU intel-iommu MMIO base
    // QEMU intel-iommu default: 0xFED90000 (if enabled via -device intel-iommu)

    uint64_t probe_bases[] = {
        0xFED90000,   // Default Intel VT-d DMAR engine base
        0xFED91000,   // Second DRHD unit (if present)
    };

    for (int i = 0; i < 2; i++) {
        // Try reading version register
        uint32_t ver = ReadReg(probe_bases[i], VTD_VER_REG);
        // Valid VT-d version: major 1-3, minor 0-9
        uint8_t major = (ver >> 4) & 0xF;
        uint8_t minor = ver & 0xF;
        if (major >= 1 && major <= 3 && minor <= 9) {
            drhd_units[drhd_count].register_base = probe_bases[i];
            drhd_units[drhd_count].valid = true;
            drhd_units[drhd_count].segment = 0;
            drhd_units[drhd_count].flags = (i == 0) ? 0 : 0;
            drhd_count++;
            if (drhd_count >= MAX_DRHD_UNITS) break;
        }
    }

    return drhd_count > 0;
}

bool IOMMU::DetectAMDVi() {
    // AMD IOMMU (AMD-Vi) detection
    // Probe known AMD IOMMU MMIO bases (typically at PCI device 0:0:2.0 or probed)
    // AMD IOMMU appears as a PCI function with class 0x0806 (IOMMU)
    // Scan PCI bus for AMD IOMMU device (vendor 0x1022, class 0x0806xx)
    for (uint8_t slot = 0; slot < 32; slot++) {
        uint32_t addr = (1u << 31) | (0u << 16) | ((uint32_t)slot << 11) | 0;
        asm volatile("outl %0, %1" : : "a"(addr), "Nd"((uint16_t)0xCF8));
        uint32_t id;
        asm volatile("inl %1, %0" : "=a"(id) : "Nd"((uint16_t)0xCFC));
        
        uint16_t vendor = id & 0xFFFF;
        if (vendor != 0x1022) continue;  // Not AMD
        
        // Read class code
        addr = (1u << 31) | (0u << 16) | ((uint32_t)slot << 11) | 0x08;
        asm volatile("outl %0, %1" : : "a"(addr), "Nd"((uint16_t)0xCF8));
        uint32_t cls;
        asm volatile("inl %1, %0" : "=a"(cls) : "Nd"((uint16_t)0xCFC));
        uint16_t class_sub = (cls >> 16) & 0xFFFF;
        
        if (class_sub == 0x0806) {
            // Found AMD IOMMU — read BAR0 for MMIO base
            addr = (1u << 31) | (0u << 16) | ((uint32_t)slot << 11) | 0x10;
            asm volatile("outl %0, %1" : : "a"(addr), "Nd"((uint16_t)0xCF8));
            uint32_t bar0;
            asm volatile("inl %1, %0" : "=a"(bar0) : "Nd"((uint16_t)0xCFC));
            
            uint64_t mmio_base = bar0 & 0xFFFFF000;
            if (mmio_base == 0) {
                // Fallback to well-known AMD IOMMU MMIO address
                mmio_base = 0xFEB00000;
            }
            
            // Try to read IOMMU capability header
            volatile uint32_t* cap = (volatile uint32_t*)mmio_base;
            uint32_t cap_hdr = *cap;
            // AMD IOMMU MMIO offset 0x00 = DeviceTable Base Address
            // Offset 0x30 = IOMMU Extended Feature Register
            if (cap_hdr != 0 && cap_hdr != 0xFFFFFFFF) {
                DRHD unit;
                unit.register_base = mmio_base;
                unit.segment = 0;
                unit.flags = 0x01;  // INCLUDE_PCI_ALL
                drhd_units[drhd_count++] = unit;
                
                SerialLogger::Log("IOMMU: AMD-Vi found at MMIO 0x");
                char hex[20]; int hi = 0;
                hex[hi++] = "0123456789ABCDEF"[(mmio_base >> 28) & 0xF];
                hex[hi++] = "0123456789ABCDEF"[(mmio_base >> 24) & 0xF];
                hex[hi++] = "0123456789ABCDEF"[(mmio_base >> 20) & 0xF];
                hex[hi++] = "0123456789ABCDEF"[(mmio_base >> 16) & 0xF];
                hex[hi++] = "0123456789ABCDEF"[(mmio_base >> 12) & 0xF];
                hex[hi++] = "0123456789ABCDEF"[(mmio_base >> 8) & 0xF];
                hex[hi++] = "0123456789ABCDEF"[(mmio_base >> 4) & 0xF];
                hex[hi++] = "0123456789ABCDEF"[mmio_base & 0xF];
                hex[hi] = 0;
                SerialLogger::Log(hex);
                SerialLogger::Log("\r\n");
                return true;
            }
        }
    }
    
    // Also try direct probe at known AMD IOMMU addresses
    const uint64_t amd_iommu_bases[] = { 0xFEB00000, 0xFEB04000, 0xFEB40000 };
    for (int i = 0; i < 3; i++) {
        volatile uint32_t* reg = (volatile uint32_t*)amd_iommu_bases[i];
        uint32_t val = *reg;
        if (val != 0 && val != 0xFFFFFFFF) {
            DRHD unit;
            unit.register_base = amd_iommu_bases[i];
            unit.segment = 0;
            unit.flags = 0x01;  // INCLUDE_PCI_ALL
            drhd_units[drhd_count++] = unit;
            return true;
        }
    }
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Root / Context Table Setup
// ═══════════════════════════════════════════════════════════════════════════

void IOMMU::SetupRootTable(uint64_t reg_base) {
    // Allocate 4KB-aligned root table (256 entries of 16 bytes = 4096 bytes)
    if (!root_table) {
        root_table = (IOMMURootEntry*)KernelHeap::Alloc(4096 + 4095);
        // Align to 4KB
        uint64_t addr = (uint64_t)root_table;
        addr = (addr + 4095) & ~(uint64_t)4095;
        root_table = (IOMMURootEntry*)addr;
        memset(root_table, 0, 4096);
    }

    // Allocate context tables (one per bus = 256 buses × 4KB each)
    // For simplicity, allocate a block for buses we actually use
    if (!context_tables) {
        // Allocate context table for bus 0 (most common)
        context_tables = (IOMMUContextEntry*)KernelHeap::Alloc(4096 + 4095);
        uint64_t addr = (uint64_t)context_tables;
        addr = (addr + 4095) & ~(uint64_t)4095;
        context_tables = (IOMMUContextEntry*)addr;
        memset(context_tables, 0, 4096);
    }

    // Point root table entry 0 (bus 0) to our context table
    uint64_t ct_phys = (uint64_t)context_tables; // Identity mapped
    root_table[0].lo = ct_phys | 0x01; // Present
    root_table[0].hi = 0;

    // Set root table address in IOMMU
    uint64_t rt_phys = (uint64_t)root_table; // Identity mapped
    WriteReg64(reg_base, VTD_RTADDR_REG, rt_phys);

    // Command: Set Root Table Pointer
    uint32_t cmd = ReadReg(reg_base, VTD_GCMD_REG);
    cmd |= VTD_GCMD_SRTP;
    WriteReg(reg_base, VTD_GCMD_REG, cmd);
    WaitForStatus(reg_base, VTD_GSTS_RTPS, true, 10000);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Translation Enable/Disable
// ═══════════════════════════════════════════════════════════════════════════

bool IOMMU::EnableTranslation() {
    if (drhd_count == 0) return false;

    for (int i = 0; i < drhd_count; i++) {
        if (!drhd_units[i].valid) continue;
        uint64_t base = drhd_units[i].register_base;

        SetupRootTable(base);

        // Enable DMA remapping
        uint32_t cmd = ReadReg(base, VTD_GCMD_REG);
        cmd |= VTD_GCMD_TE;
        WriteReg(base, VTD_GCMD_REG, cmd);

        if (!WaitForStatus(base, VTD_GSTS_TES, true, 100000)) {
            SerialLogger::Log("[IOMMU] Failed to enable translation on DRHD unit\r\n");
            return false;
        }
    }

    iommu_state = IOMMU_STATE_ACTIVE;
    SerialLogger::Log("[IOMMU] DMA remapping enabled\r\n");
    return true;
}

bool IOMMU::DisableTranslation() {
    if (drhd_count == 0) return false;

    for (int i = 0; i < drhd_count; i++) {
        if (!drhd_units[i].valid) continue;
        uint64_t base = drhd_units[i].register_base;

        uint32_t cmd = ReadReg(base, VTD_GCMD_REG);
        cmd &= ~VTD_GCMD_TE;
        WriteReg(base, VTD_GCMD_REG, cmd);
        WaitForStatus(base, VTD_GSTS_TES, false, 100000);
    }

    iommu_state = IOMMU_STATE_ENABLED;
    return true;
}

void IOMMU::FlushIOTLB() {
    for (int i = 0; i < drhd_count; i++) {
        if (!drhd_units[i].valid) continue;
        uint64_t base = drhd_units[i].register_base;

        // Global IOTLB invalidate
        uint64_t cap = ReadReg64(base, VTD_CAP_REG);
        uint32_t iro = (uint32_t)((cap >> 8) & 0x3FF) * 16; // IOTLB register offset

        // Write IVT (Invalidate) + global granularity + drain reads/writes
        uint64_t iotlb_cmd = (uint64_t)1 << 63; // IVT
        iotlb_cmd |= (uint64_t)0x01 << 60;       // Global invalidation
        iotlb_cmd |= (uint64_t)0x01 << 49;       // Drain reads
        iotlb_cmd |= (uint64_t)0x01 << 48;       // Drain writes

        WriteReg64(base, iro + 8, iotlb_cmd);

        // Wait for completion (IVT bit clears)
        for (int j = 0; j < 10000; j++) {
            if (!(ReadReg64(base, iro + 8) & ((uint64_t)1 << 63))) break;
            for (volatile int k = 0; k < 100; k++) {}
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Device Assignment
// ═══════════════════════════════════════════════════════════════════════════

bool IOMMU::AssignDevice(uint8_t bus, uint8_t dev, uint8_t func, uint16_t domain_id) {
    if (assign_count >= MAX_DEVICE_ASSIGNS) return false;
    if (!context_tables) return false;

    // Only bus 0 context table allocated for now
    if (bus != 0) {
        SerialLogger::Log("[IOMMU] Only bus 0 supported currently\r\n");
        return false;
    }

    // Context entry index = (dev << 3) | func
    int ctx_idx = (dev << 3) | func;

    // Set up context entry for identity mapping (pass-through)
    // Translation type 0x02 = pass-through (identity 1:1 mapping)
    uint64_t lo = 0;
    lo |= 0x01;                                 // Present
    lo |= ((uint64_t)0x02 << 2);                // Translation type: pass-through
    lo |= ((uint64_t)(domain_id & 0xFFFF) << 8); // Domain ID

    uint64_t hi = 0;
    hi |= ((uint64_t)0x02 << 0);                // Address width: 48-bit AGAW (4-level)

    context_tables[ctx_idx].lo = lo;
    context_tables[ctx_idx].hi = hi;

    // Record assignment
    assigned[assign_count].bus = bus;
    assigned[assign_count].dev = dev;
    assigned[assign_count].func = func;
    assigned[assign_count].domain_id = domain_id;
    assigned[assign_count].active = true;
    assign_count++;

    // Flush context cache and IOTLB
    FlushIOTLB();

    return true;
}

bool IOMMU::UnassignDevice(uint8_t bus, uint8_t dev, uint8_t func) {
    for (int i = 0; i < assign_count; i++) {
        if (assigned[i].bus == bus && assigned[i].dev == dev &&
            assigned[i].func == func && assigned[i].active) {
            assigned[i].active = false;

            // Clear context entry
            if (bus == 0 && context_tables) {
                int ctx_idx = (dev << 3) | func;
                context_tables[ctx_idx].lo = 0;
                context_tables[ctx_idx].hi = 0;
            }
            FlushIOTLB();
            return true;
        }
    }
    return false;
}

bool IOMMU::IsDeviceAssigned(uint8_t bus, uint8_t dev, uint8_t func) {
    for (int i = 0; i < assign_count; i++) {
        if (assigned[i].bus == bus && assigned[i].dev == dev &&
            assigned[i].func == func && assigned[i].active)
            return true;
    }
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Identity Domain (for DMA passthrough)
// ═══════════════════════════════════════════════════════════════════════════

bool IOMMU::SetupIdentityDomain(uint16_t domain_id, uint64_t phys_start, uint64_t phys_size) {
    // In pass-through mode (translation type 0x02), the IOMMU performs
    // identity 1:1 mapping — no page tables needed.
    // Just record the domain for tracking purposes.
    (void)domain_id;
    (void)phys_start;
    (void)phys_size;
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
//  NVIDIA GPU Passthrough
// ═══════════════════════════════════════════════════════════════════════════

bool IOMMU::SetupGPUPassthrough() {
    if (!IsSupported()) {
        SerialLogger::Log("[IOMMU] VT-d not available — cannot passthrough GPU\r\n");
        return false;
    }

    if (!NvidiaGPU::IsDetected()) {
        SerialLogger::Log("[IOMMU] No NVIDIA GPU detected — cannot passthrough\r\n");
        return false;
    }

    const NvidiaGPUInfo& info = NvidiaGPU::GetInfo();

    SerialLogger::Log("[IOMMU] Setting up GPU passthrough for ");
    SerialLogger::Log(info.name);
    SerialLogger::Log("\r\n");

    // 1. Prepare GPU for passthrough (disable host driver interrupts, bus master)
    NvidiaGPU::PrepareForPassthrough();

    // 2. Enable DMA remapping if not already active
    if (iommu_state != IOMMU_STATE_ACTIVE) {
        if (!EnableTranslation()) {
            SerialLogger::Log("[IOMMU] Failed to enable DMA remapping\r\n");
            return false;
        }
    }

    // 3. Assign GPU to passthrough domain (domain ID 1)
    if (!AssignDevice(info.bus, info.device, info.function, 1)) {
        SerialLogger::Log("[IOMMU] Failed to assign GPU to IOMMU domain\r\n");
        return false;
    }

    SerialLogger::Log("[IOMMU] GPU passthrough active — device assigned to domain 1\r\n");
    return true;
}

bool IOMMU::TeardownGPUPassthrough() {
    if (!NvidiaGPU::IsDetected()) return false;
    const NvidiaGPUInfo& info = NvidiaGPU::GetInfo();

    UnassignDevice(info.bus, info.device, info.function);
    SerialLogger::Log("[IOMMU] GPU passthrough teardown complete\r\n");
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Initialization
// ═══════════════════════════════════════════════════════════════════════════

void IOMMU::Init() {
    type = IOMMU_NONE;
    iommu_state = IOMMU_STATE_OFF;
    drhd_count = 0;
    assign_count = 0;
    root_table = nullptr;
    context_tables = nullptr;

    SerialLogger::Log("[IOMMU] Initializing IOMMU subsystem...\r\n");

    // Try Intel VT-d first
    if (DetectVTd()) {
        type = IOMMU_INTEL_VTD;
        iommu_state = IOMMU_STATE_DETECTED;

        // Read capabilities of first DRHD
        uint64_t base = drhd_units[0].register_base;
        uint32_t ver = ReadReg(base, VTD_VER_REG);
        uint8_t major = (ver >> 4) & 0xF;
        uint8_t minor = ver & 0xF;

        SerialLogger::Log("[IOMMU] Intel VT-d detected, version ");
        char vbuf[8];
        vbuf[0] = '0' + major; vbuf[1] = '.'; vbuf[2] = '0' + minor; vbuf[3] = 0;
        SerialLogger::Log(vbuf);
        SerialLogger::Log("\r\n");

        uint64_t cap = ReadReg64(base, VTD_CAP_REG);
        uint64_t ecap = ReadReg64(base, VTD_ECAP_REG);
        (void)cap; (void)ecap;

        iommu_state = IOMMU_STATE_ENABLED;
        SerialLogger::Log("[IOMMU] VT-d ready for device passthrough\r\n");
        return;
    }

    // Try AMD-Vi
    if (DetectAMDVi()) {
        type = IOMMU_AMD_VI;
        iommu_state = IOMMU_STATE_DETECTED;
        SerialLogger::Log("[IOMMU] AMD-Vi detected\r\n");
        return;
    }

    SerialLogger::Log("[IOMMU] No IOMMU hardware detected\r\n");
}

bool IOMMU::IsSupported() {
    return type != IOMMU_NONE;
}

IOMMUType IOMMU::GetType() {
    return type;
}

IOMMUState IOMMU::GetState() {
    return iommu_state;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Debug / Status
// ═══════════════════════════════════════════════════════════════════════════

void IOMMU::DumpInfo(char* out, int maxo) {
    int p = 0;
    p = iommu_sa(out, p, maxo, "\033[36m═══ IOMMU Status ═══\033[0m\n");
    p = iommu_sa(out, p, maxo, "  Type : ");
    switch (type) {
        case IOMMU_INTEL_VTD: p = iommu_sa(out, p, maxo, "Intel VT-d"); break;
        case IOMMU_AMD_VI:    p = iommu_sa(out, p, maxo, "AMD-Vi"); break;
        default:              p = iommu_sa(out, p, maxo, "None"); break;
    }
    p = iommu_sa(out, p, maxo, "\n");

    p = iommu_sa(out, p, maxo, "  State: ");
    switch (iommu_state) {
        case IOMMU_STATE_DETECTED: p = iommu_sa(out, p, maxo, "Detected"); break;
        case IOMMU_STATE_ENABLED:  p = iommu_sa(out, p, maxo, "Enabled"); break;
        case IOMMU_STATE_ACTIVE:   p = iommu_sa(out, p, maxo, "Active (DMA remapping on)"); break;
        case IOMMU_STATE_ERROR:    p = iommu_sa(out, p, maxo, "Error"); break;
        default:                   p = iommu_sa(out, p, maxo, "Off"); break;
    }
    p = iommu_sa(out, p, maxo, "\n");

    if (drhd_count > 0) {
        p = iommu_sa(out, p, maxo, "  DRHD units: ");
        char nb[4]; nb[0] = '0' + drhd_count; nb[1] = 0;
        p = iommu_sa(out, p, maxo, nb);
        p = iommu_sa(out, p, maxo, "\n");
        for (int i = 0; i < drhd_count; i++) {
            p = iommu_sa(out, p, maxo, "    ["); nb[0] = '0' + i; p = iommu_sa(out, p, maxo, nb);
            p = iommu_sa(out, p, maxo, "] base=0x");
            p = iommu_hex(out, p, maxo, drhd_units[i].register_base);
            p = iommu_sa(out, p, maxo, "\n");
        }
    }

    if (assign_count > 0) {
        p = iommu_sa(out, p, maxo, "  Assigned devices:\n");
        for (int i = 0; i < assign_count; i++) {
            if (!assigned[i].active) continue;
            p = iommu_sa(out, p, maxo, "    PCI ");
            p = iommu_hex(out, p, maxo, assigned[i].bus); p = iommu_sa(out, p, maxo, ":");
            p = iommu_hex(out, p, maxo, assigned[i].dev); p = iommu_sa(out, p, maxo, ".");
            p = iommu_hex(out, p, maxo, assigned[i].func);
            p = iommu_sa(out, p, maxo, " → domain ");
            p = iommu_hex(out, p, maxo, assigned[i].domain_id);
            p = iommu_sa(out, p, maxo, "\n");
        }
    }
}
