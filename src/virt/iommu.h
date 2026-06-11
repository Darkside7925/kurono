#pragma once
// ═══════════════════════════════════════════════════════════════════════════
//  Kurono OS — Intel VT-d / IOMMU Support
//  DMA remapping for PCI device passthrough to virtual machines
//  Enables GPU passthrough (e.g., NVIDIA RTX 5090 → Linux guest)
// ═══════════════════════════════════════════════════════════════════════════
#include "../kernel/types.h"

// ── IOMMU Capability Detection ──
enum IOMMUType {
    IOMMU_NONE = 0,
    IOMMU_INTEL_VTD,    // Intel VT-d (Virtualization Technology for Directed I/O)
    IOMMU_AMD_VI,       // AMD-Vi (I/O Virtualization)
};

// ── IOMMU State ──
enum IOMMUState {
    IOMMU_STATE_OFF = 0,
    IOMMU_STATE_DETECTED,
    IOMMU_STATE_ENABLED,
    IOMMU_STATE_ACTIVE,     // DMA remapping active
    IOMMU_STATE_ERROR,
};

// ── DMA Remapping Hardware Unit (DRHD) — from ACPI DMAR table ──
struct DRHD {
    uint8_t     flags;          // Bit 0: INCLUDE_PCI_ALL
    uint16_t    segment;        // PCI segment number
    uint64_t    register_base;  // MMIO base of this remapping unit
    bool        valid;
};

// ── IOMMU Context Entry (maps BDF → domain page table) ──
struct IOMMUContextEntry {
    uint64_t    lo;     // Present, fault-disable, translation-type, domain ID
    uint64_t    hi;     // Address-width, second-level page table pointer
} __attribute__((packed));

// ── IOMMU Root Entry (one per PCI bus) ──
struct IOMMURootEntry {
    uint64_t    lo;     // Present bit + context table pointer
    uint64_t    hi;     // Reserved
} __attribute__((packed));

// ── Device Assignment Record ──
struct DeviceAssignment {
    uint8_t     bus, dev, func;     // PCI BDF
    uint16_t    domain_id;          // IOMMU domain
    uint64_t    page_table_root;    // Guest physical → host physical mapping
    bool        active;
};

// ── VT-d Register Offsets ──
#define VTD_VER_REG         0x000   // Version
#define VTD_CAP_REG         0x008   // Capability
#define VTD_ECAP_REG        0x010   // Extended Capability
#define VTD_GCMD_REG        0x018   // Global Command
#define VTD_GSTS_REG        0x01C   // Global Status
#define VTD_RTADDR_REG      0x020   // Root Table Address
#define VTD_CCMD_REG        0x028   // Context Command
#define VTD_FSTS_REG        0x034   // Fault Status
#define VTD_FECTL_REG       0x038   // Fault Event Control
#define VTD_FEDATA_REG      0x03C   // Fault Event Data
#define VTD_FEADDR_REG      0x040   // Fault Event Address
#define VTD_FEUADDR_REG     0x044   // Fault Event Upper Address
#define VTD_IOTLB_REG       0x108   // IOTLB Invalidate

// ── VT-d Global Command Bits ──
#define VTD_GCMD_TE         (1u << 31)  // Translation Enable
#define VTD_GCMD_SRTP       (1u << 30)  // Set Root Table Pointer
#define VTD_GCMD_SFL        (1u << 29)  // Set Fault Log
#define VTD_GCMD_EAFL       (1u << 28)  // Enable Advanced Fault Logging
#define VTD_GCMD_WBF        (1u << 27)  // Write Buffer Flush
#define VTD_GCMD_IRE        (1u << 25)  // Interrupt Remapping Enable

// ── VT-d Global Status Bits ──
#define VTD_GSTS_TES        (1u << 31)  // Translation Enable Status
#define VTD_GSTS_RTPS       (1u << 30)  // Root Table Pointer Status
#define VTD_GSTS_IRES       (1u << 25)  // Interrupt Remapping Enable Status

// ── Max Supported Resources ──
#define MAX_DRHD_UNITS      8
#define MAX_DEVICE_ASSIGNS  16

// ═══════════════════════════════════════════════════════════════════════════
//  IOMMU — Static driver interface for Intel VT-d / AMD-Vi
// ═══════════════════════════════════════════════════════════════════════════
class IOMMU {
public:
    // ── Lifecycle ──
    static void Init();                             // Detect & initialize IOMMU
    static bool IsSupported();                      // VT-d/AMD-Vi available?
    static IOMMUType GetType();
    static IOMMUState GetState();

    // ── DMA Remapping ──
    static bool EnableTranslation();                // Enable DMA remapping globally
    static bool DisableTranslation();
    static void FlushIOTLB();                       // Invalidate IOTLB

    // ── Device Assignment (Passthrough) ──
    static bool AssignDevice(uint8_t bus, uint8_t dev, uint8_t func, uint16_t domain_id);
    static bool UnassignDevice(uint8_t bus, uint8_t dev, uint8_t func);
    static bool IsDeviceAssigned(uint8_t bus, uint8_t dev, uint8_t func);

    // ── NVIDIA GPU Passthrough ──
    static bool SetupGPUPassthrough();              // Assign detected NVIDIA GPU
    static bool TeardownGPUPassthrough();

    // ── Identity Map for Direct DMA ──
    static bool SetupIdentityDomain(uint16_t domain_id, uint64_t phys_start, uint64_t phys_size);

    // ── Debug / Status ──
    static void DumpInfo(char* out, int maxo);

private:
    static IOMMUType       type;
    static IOMMUState      iommu_state;
    static DRHD            drhd_units[MAX_DRHD_UNITS];
    static int             drhd_count;
    static DeviceAssignment assigned[MAX_DEVICE_ASSIGNS];
    static int             assign_count;

    // ── Root/Context Tables ──
    static IOMMURootEntry*   root_table;     // 4KB-aligned, 256 entries
    static IOMMUContextEntry* context_tables; // One per bus, 256 entries each

    // ── VT-d Register Access ──
    static uint32_t ReadReg(uint64_t base, uint32_t offset);
    static void     WriteReg(uint64_t base, uint32_t offset, uint32_t val);
    static uint64_t ReadReg64(uint64_t base, uint32_t offset);
    static void     WriteReg64(uint64_t base, uint32_t offset, uint64_t val);

    // ── Internal ──
    static bool DetectVTd();                        // Check CPUID + ACPI DMAR
    static bool DetectAMDVi();                      // Check CPUID + ACPI IVRS
    static void SetupRootTable(uint64_t reg_base);
    static bool WaitForStatus(uint64_t base, uint32_t bit, bool set, int timeout_us);
};
