//  kurono os  -  pci device passthrough orchestrator (path b)
//
//  routes a real host pci device (e.g. a discrete nvidia or amd gpu) into
//  the alpine guest so the upstream linux nvidia/amdgpu .ko can drive
//  actual silicon.
//
//  responsibilities:
//    1. release the device from kurono's native driver (nvidia_gpu /
//       amd_gpu) so we stop poking its mmio.
//    2. set up an iommu domain that identity-maps the device's dma into
//       host physical pages backing the guest ram.
//    3. register the device on vpci with bar callbacks that proxy real
//       mmio + irq events to/from the guest.
//    4. ept-map the host bar phys → guest phys passthrough region so guest
//       loads/stores hit the real device with no vm-exit.
//    5. forward the device's host irq into the guest as a virtual
//       interrupt vector (msi or legacy intx).
//
//  this module is a thin orchestrator  -  the actual heavy lifting (iommu
//  page tables, ept mapping, irq registration) lives in iommu.cpp /
//  ept.cpp / hal.cpp and is invoked from here.
//
//  usage from shell:
//      vgpu passthrough nvidia    -> hand off discrete nvidia gpu to guest
//      vgpu passthrough amd       -> hand off amd radeon gpu to guest
//      vgpu reclaim               -> return all passthrough devices to host
#pragma once
#include "../kernel/types.h"

enum PCIPassthroughKind {
    PT_KIND_NONE = 0,
    PT_KIND_NVIDIA_GPU,
    PT_KIND_AMD_GPU,
    PT_KIND_GENERIC_PCI,
};

struct PCIPassthroughEntry {
    bool                in_use;
    PCIPassthroughKind  kind;
    uint8_t             host_bus;
    uint8_t             host_dev;
    uint8_t             host_func;
    uint8_t             guest_slot;       // vpci slot exposed to guest
    uint16_t            domain_id;        // iommu domain
    uint64_t            bar_host_phys[6]; // real host bar bases
    uint64_t            bar_guest_phys[6];// guest-visible bar bases (vpci alloc)
    uint32_t            bar_size[6];
    uint8_t             host_irq;
    uint8_t             guest_irq;
};

constexpr int MAX_PT_DEVICES = 4;

class PCIPassthrough {
public:
    static void Init();

    // hand off a nvidia/amd gpu detected at boot into the guest.
    // returns false if iommu is not enabled, no gpu detected, or vpci
    // registration fails.
    static bool HandoffNvidiaGPU();
    static bool HandoffAmdGPU();

    // reclaim a previously-passthrough'd device back to the host
    static bool Reclaim(uint8_t bus, uint8_t dev, uint8_t func);
    static void ReclaimAll();

    // status / debug
    static int  Count();
    static const PCIPassthroughEntry* Get(int slot);
    static void DumpStatus(char* out, int maxo);

    // delivered by hal when a passthrough device's real irq fires
    //  -  forwards to hypervisor as guest interrupt injection
    static void OnHostIRQ(uint8_t host_irq);

private:
    static PCIPassthroughEntry entries[MAX_PT_DEVICES];
    static int                 entry_count;

    // common path: register `e` on vpci with proxy callbacks
    static bool RegisterOnVPCI(PCIPassthroughEntry* e,
                                 const char* name, uint16_t vendor,
                                 uint16_t device, uint8_t pci_class);

    // guest reads/writes a bar on a passthrough device  -  forward to
    // the real mmio at host_phys + off (we just identity-mapped it
    // into ept so this should be unreachable from the fast path; this
    // exists for size-probe and unmapped accesses)
    static bool BarRead (struct VPCIDevice* dev, int bar, uint32_t off,
                          uint8_t size, uint32_t& value);
    static bool BarWrite(struct VPCIDevice* dev, int bar, uint32_t off,
                          uint8_t size, uint32_t value);
};
