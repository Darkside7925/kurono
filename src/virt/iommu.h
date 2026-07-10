#pragma once
//  kurono os - intel vt-d / iommu support
//  dma remapping for pci device passthrough to virtual machines
//  enables gpu passthrough (e.g., nvidia rtx 5090 → linux guest)
#include "../kernel/types.h"

enum IOMMUType {
    IOMMU_NONE = 0,
    IOMMU_INTEL_VTD,    // intel vt-d (virtualization technology for directed i/o)
    IOMMU_AMD_VI,       // amd-vi (i/o virtualization)
};

enum IOMMUState {
    IOMMU_STATE_OFF = 0,
    IOMMU_STATE_DETECTED,
    IOMMU_STATE_ENABLED,
    IOMMU_STATE_ACTIVE,     // dma remapping active
    IOMMU_STATE_ERROR,
};

struct DRHD {
    uint8_t     flags;          // bit 0: include_pci_all
    uint16_t    segment;        // pci segment number
    uint64_t    register_base;  // mmio base of this remapping unit
    bool        valid;
};

struct IOMMUContextEntry {
    uint64_t    lo;     // present, fault-disable, translation-type, domain id
    uint64_t    hi;     // address-width, second-level page table pointer
} __attribute__((packed));

struct IOMMURootEntry {
    uint64_t    lo;     // present bit + context table pointer
    uint64_t    hi;     // reserved
} __attribute__((packed));

struct DeviceAssignment {
    uint8_t     bus, dev, func;     // pci bdf
    uint16_t    domain_id;          // iommu domain
    uint64_t    page_table_root;    // guest physical → host physical mapping
    bool        active;
};

#define VTD_VER_REG         0x000   // version
#define VTD_CAP_REG         0x008   // capability
#define VTD_ECAP_REG        0x010   // extended capability
#define VTD_GCMD_REG        0x018   // global command
#define VTD_GSTS_REG        0x01C   // global status
#define VTD_RTADDR_REG      0x020   // root table address
#define VTD_CCMD_REG        0x028   // context command
#define VTD_FSTS_REG        0x034   // fault status
#define VTD_FECTL_REG       0x038   // fault event control
#define VTD_FEDATA_REG      0x03C   // fault event data
#define VTD_FEADDR_REG      0x040   // fault event address
#define VTD_FEUADDR_REG     0x044   // fault event upper address
#define VTD_IOTLB_REG       0x108   // iotlb invalidate

#define VTD_GCMD_TE         (1u << 31)  // translation enable
#define VTD_GCMD_SRTP       (1u << 30)  // set root table pointer
#define VTD_GCMD_SFL        (1u << 29)  // set fault log
#define VTD_GCMD_EAFL       (1u << 28)  // enable advanced fault logging
#define VTD_GCMD_WBF        (1u << 27)  // write buffer flush
#define VTD_GCMD_IRE        (1u << 25)  // interrupt remapping enable

#define VTD_GSTS_TES        (1u << 31)  // translation enable status
#define VTD_GSTS_RTPS       (1u << 30)  // root table pointer status
#define VTD_GSTS_IRES       (1u << 25)  // interrupt remapping enable status

#define MAX_DRHD_UNITS      8
#define MAX_DEVICE_ASSIGNS  16

//  iommu - static driver interface for intel vt-d / amd-vi
class IOMMU {
public:
    static void Init();                             // detect & initialize iommu
    static bool IsSupported();                      // vt-d/amd-vi available?
    static IOMMUType GetType();
    static IOMMUState GetState();

    static bool EnableTranslation();                // enable dma remapping globally
    static bool DisableTranslation();
    static void FlushIOTLB();                       // invalidate iotlb

    static bool AssignDevice(uint8_t bus, uint8_t dev, uint8_t func, uint16_t domain_id);
    static bool UnassignDevice(uint8_t bus, uint8_t dev, uint8_t func);
    static bool IsDeviceAssigned(uint8_t bus, uint8_t dev, uint8_t func);

    static bool SetupGPUPassthrough();              // assign detected nvidia gpu
    static bool TeardownGPUPassthrough();

    static bool SetupIdentityDomain(uint16_t domain_id, uint64_t phys_start, uint64_t phys_size);

    static void DumpInfo(char* out, int maxo);

private:
    static IOMMUType       type;
    static IOMMUState      iommu_state;
    static DRHD            drhd_units[MAX_DRHD_UNITS];
    static int             drhd_count;
    static DeviceAssignment assigned[MAX_DEVICE_ASSIGNS];
    static int             assign_count;

    static IOMMURootEntry*   root_table;     // 4kb-aligned, 256 entries
    static IOMMUContextEntry* context_tables; // one per bus, 256 entries each

    static uint32_t ReadReg(uint64_t base, uint32_t offset);
    static void     WriteReg(uint64_t base, uint32_t offset, uint32_t val);
    static uint64_t ReadReg64(uint64_t base, uint32_t offset);
    static void     WriteReg64(uint64_t base, uint32_t offset, uint64_t val);

    static bool DetectVTd();                        // check cpuid + acpi dmar
    static bool DetectAMDVi();                      // check cpuid + acpi ivrs
    static void SetupRootTable(uint64_t reg_base);
    static bool WaitForStatus(uint64_t base, uint32_t bit, bool set, int timeout_us);
};
