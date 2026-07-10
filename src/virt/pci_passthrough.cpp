//  kurono os - pci device passthrough orchestrator (path b)
//  see pci_passthrough.h for design.
#include "pci_passthrough.h"
#include "vpci.h"
#include "iommu.h"
#include "ept.h"
#include "guest_mem.h"
#include "hypervisor.h"
#include "../drivers/serial.h"
#include "../drivers/nvidia_gpu.h"
#include "../drivers/amd_gpu.h"
#include "../hal/hal.h"

PCIPassthroughEntry PCIPassthrough::entries[MAX_PT_DEVICES];
int                 PCIPassthrough::entry_count = 0;

void PCIPassthrough::Init() {
    for (int i = 0; i < MAX_PT_DEVICES; i++) entries[i] = {};
    entry_count = 0;
}

// ----------------------------------------------------------------- vpci proxy
bool PCIPassthrough::BarRead(VPCIDevice* dev, int bar, uint32_t off,
                                uint8_t size, uint32_t& value) {
    // find which passthrough entry owns this guest device
    for (int i = 0; i < entry_count; i++) {
        PCIPassthroughEntry& e = entries[i];
        if (!e.in_use || e.guest_slot != dev->dev) continue;
        if (bar < 0 || bar >= 6) return false;
        uint64_t host_phys = e.bar_host_phys[bar];
        if (host_phys == 0) { value = 0xFFFFFFFFu; return true; }
        // mmio is identity-mapped in kurono's address space - we read
        // directly.  Note: this fallback path runs only when the guest
        // accesses a bar offset that wasn't ept-mapped (e.g. during
        // initial size probe).
        volatile uint8_t* p = (volatile uint8_t*)(host_phys + off);
        switch (size) {
            case 1: value = *p; break;
            case 2: value = *(volatile uint16_t*)p; break;
            default:value = *(volatile uint32_t*)p; break;
        }
        return true;
    }
    return false;
}

bool PCIPassthrough::BarWrite(VPCIDevice* dev, int bar, uint32_t off,
                                 uint8_t size, uint32_t value) {
    for (int i = 0; i < entry_count; i++) {
        PCIPassthroughEntry& e = entries[i];
        if (!e.in_use || e.guest_slot != dev->dev) continue;
        if (bar < 0 || bar >= 6) return false;
        uint64_t host_phys = e.bar_host_phys[bar];
        if (host_phys == 0) return true;
        volatile uint8_t* p = (volatile uint8_t*)(host_phys + off);
        switch (size) {
            case 1: *p = (uint8_t)value; break;
            case 2: *(volatile uint16_t*)p = (uint16_t)value; break;
            default:*(volatile uint32_t*)p = value; break;
        }
        return true;
    }
    return false;
}

// ----------------------------------------------------------------- registration
bool PCIPassthrough::RegisterOnVPCI(PCIPassthroughEntry* e,
                                       const char* name, uint16_t vendor,
                                       uint16_t device, uint8_t pci_class) {
    VPCIDevice d{};
    d.name     = name;
    d.irq_line = (e->host_irq) ? e->host_irq : 11;
    VPCI::Cfg16(&d, PCI_VENDOR_ID, vendor);
    VPCI::Cfg16(&d, PCI_DEVICE_ID, device);
    VPCI::Cfg8 (&d, PCI_REVISION_ID, 0xA1);
    VPCI::Cfg8 (&d, PCI_CLASS,      pci_class);
    VPCI::Cfg8 (&d, PCI_SUBCLASS,   0x00);
    VPCI::Cfg8 (&d, PCI_PROG_IF,    0x00);
    VPCI::Cfg16(&d, PCI_SUBSYS_VEND, vendor);
    VPCI::Cfg16(&d, PCI_SUBSYS_ID,   device);

    // mirror bar sizes - vpci will assign new guest-phys bases inside
    // its mmio window.  on a future EPTManager::MapMMIO call we'll
    // map host_phys → those guest bases so the guest's loads bypass
    // vm-exits entirely.
    for (int b = 0; b < 6; b++) {
        if (e->bar_size[b] == 0) continue;
        d.bars[b].size     = e->bar_size[b];
        d.bars[b].is_mmio  = true;
        d.bars[b].is_64bit = (e->bar_size[b] >= 0x10000); // assume 64-bit for big ones
        d.bars[b].prefetch = true;
    }
    d.bar_read   = &PCIPassthrough::BarRead;
    d.bar_write  = &PCIPassthrough::BarWrite;
    d.cfg_notify = nullptr;

    int slot = VPCI::RegisterDevice(&d);
    if (slot < 0) return false;
    e->guest_slot = (uint8_t)slot;

    // identity-map the real host bar phys into the guest physical
    // window vpci just assigned.  this is the fast path: subsequent
    // guest loads/stores on the BAR bypass the vmm entirely.
    VPCIDevice* canonical = VPCI::GetDevice(slot);
    if (canonical) {
        for (int b = 0; b < 6; b++) {
            if (canonical->bars[b].size == 0) continue;
            uint64_t guest_phys = canonical->bars[b].base;
            uint64_t host_phys  = e->bar_host_phys[b];
            e->bar_guest_phys[b] = guest_phys;
            EPTManager::AddRegion({guest_phys, host_phys,
                                    (uint32_t)canonical->bars[b].size,
                                    MEM_MMIO, true, true, false});
        }
    }
    return true;
}

// ----------------------------------------------------------------- nvidia
bool PCIPassthrough::HandoffNvidiaGPU() {
    if (!IOMMU::IsSupported()) {
        SerialLogger::Log("PCIPassthrough: IOMMU not available - refusing nvidia handoff\r\n");
        return false;
    }
    if (!NvidiaGPU::IsDetected()) {
        SerialLogger::Log("PCIPassthrough: no nvidia gpu detected\r\n");
        return false;
    }
    if (entry_count >= MAX_PT_DEVICES) return false;
    const NvidiaGPUInfo& info = NvidiaGPU::GetInfo();

    // 1. release host driver
    NvidiaGPU::PrepareForPassthrough();

    // 2. iommu domain assignment (identity)
    if (!IOMMU::AssignDevice(info.bus, info.device, info.function, 1)) {
        SerialLogger::Log("PCIPassthrough: iommu assign failed\r\n");
        return false;
    }

    // 3. populate entry
    PCIPassthroughEntry* e = &entries[entry_count];
    *e = {};
    e->in_use   = true;
    e->kind     = PT_KIND_NVIDIA_GPU;
    e->host_bus = info.bus;
    e->host_dev = info.device;
    e->host_func= info.function;
    e->domain_id= 1;
    e->bar_host_phys[0] = info.bar0;
    e->bar_size[0]      = (uint32_t)info.bar0_size;
    e->bar_host_phys[1] = info.bar1;
    e->bar_size[1]      = (uint32_t)info.bar1_size;
    // legacy intx line - read from real device
    uint32_t intl = NvidiaGPU::PciRead(info.bus, info.device, info.function, 0x3C);
    e->host_irq = (uint8_t)(intl & 0xFF);

    // 4. expose to guest via vpci
    if (!RegisterOnVPCI(e, "nvidia-passthrough",
                          info.vendor_id, info.device_id, 0x03)) {
        SerialLogger::Log("PCIPassthrough: vpci register failed\r\n");
        IOMMU::UnassignDevice(info.bus, info.device, info.function);
        return false;
    }

    entry_count++;
    SerialLogger::Log("PCIPassthrough: nvidia gpu handed off to guest\r\n");
    return true;
}

// ----------------------------------------------------------------- amd
bool PCIPassthrough::HandoffAmdGPU() {
    if (!IOMMU::IsSupported()) {
        SerialLogger::Log("PCIPassthrough: IOMMU not available - refusing amd handoff\r\n");
        return false;
    }
    if (!AmdGPU::IsAvailable()) {
        SerialLogger::Log("PCIPassthrough: no amd gpu detected\r\n");
        return false;
    }
    if (entry_count >= MAX_PT_DEVICES) return false;
    const AmdGPUInfo& info = AmdGPU::GetInfo();

    if (!IOMMU::AssignDevice(info.bus, info.device, info.function, 2)) {
        SerialLogger::Log("PCIPassthrough: iommu assign failed\r\n");
        return false;
    }

    PCIPassthroughEntry* e = &entries[entry_count];
    *e = {};
    e->in_use   = true;
    e->kind     = PT_KIND_AMD_GPU;
    e->host_bus = info.bus;
    e->host_dev = info.device;
    e->host_func= info.function;
    e->domain_id= 2;
    e->bar_host_phys[0] = info.bar0;
    e->bar_size[0]      = (uint32_t)info.bar0_size;
    e->bar_host_phys[2] = info.vram_bar;
    e->bar_size[2]      = 0x10000000; // 256 mb visible aperture by default

    if (!RegisterOnVPCI(e, "amdgpu-passthrough",
                          info.vendor_id, info.device_id, 0x03)) {
        IOMMU::UnassignDevice(info.bus, info.device, info.function);
        return false;
    }

    entry_count++;
    SerialLogger::Log("PCIPassthrough: amd gpu handed off to guest\r\n");
    return true;
}

// ----------------------------------------------------------------- reclaim
bool PCIPassthrough::Reclaim(uint8_t bus, uint8_t dev, uint8_t func) {
    for (int i = 0; i < entry_count; i++) {
        PCIPassthroughEntry& e = entries[i];
        if (!e.in_use) continue;
        if (e.host_bus == bus && e.host_dev == dev && e.host_func == func) {
            IOMMU::UnassignDevice(bus, dev, func);
            e.in_use = false;
            return true;
        }
    }
    return false;
}

void PCIPassthrough::ReclaimAll() {
    for (int i = 0; i < entry_count; i++) {
        PCIPassthroughEntry& e = entries[i];
        if (!e.in_use) continue;
        IOMMU::UnassignDevice(e.host_bus, e.host_dev, e.host_func);
        e.in_use = false;
    }
    entry_count = 0;
}

// ----------------------------------------------------------------- irq fwd
void PCIPassthrough::OnHostIRQ(uint8_t host_irq) {
    // when a passthrough device's real interrupt fires, inject a guest
    // vector into the running vm.  we map host irq → guest irq vector
    // 0x40 + host_irq for now (gives us 32-47 mapped to PIC IRQs).
    for (int i = 0; i < entry_count; i++) {
        PCIPassthroughEntry& e = entries[i];
        if (!e.in_use) continue;
        if (e.host_irq == host_irq) {
            uint8_t vector = (uint8_t)(0x40 + host_irq);
            Hypervisor::InjectInterrupt(vector);
        }
    }
}

// ----------------------------------------------------------------- status
int PCIPassthrough::Count() {
    int n = 0;
    for (int i = 0; i < entry_count; i++) if (entries[i].in_use) n++;
    return n;
}

const PCIPassthroughEntry* PCIPassthrough::Get(int slot) {
    if (slot < 0 || slot >= entry_count) return nullptr;
    return &entries[slot];
}

static int _pt_cat(char* out, int p, int max, const char* s) {
    while (*s && p < max - 1) out[p++] = *s++;
    if (p < max) out[p] = 0;
    return p;
}
static int _pt_dec(char* out, int p, int max, uint32_t v) {
    char tmp[12]; int n = 0;
    if (v == 0) { if (p < max - 1) out[p++] = '0'; return p; }
    while (v > 0 && n < 11) { tmp[n++] = '0' + (v % 10); v /= 10; }
    while (n > 0 && p < max - 1) out[p++] = tmp[--n];
    return p;
}
static int _pt_hex(char* out, int p, int max, uint64_t v) {
    char tmp[20]; int n = 0;
    if (v == 0) { if (p < max - 1) out[p++] = '0'; return p; }
    while (v > 0 && n < 16) { tmp[n++] = "0123456789abcdef"[v & 0xF]; v >>= 4; }
    while (n > 0 && p < max - 1) out[p++] = tmp[--n];
    return p;
}

void PCIPassthrough::DumpStatus(char* out, int maxo) {
    int p = 0;
    p = _pt_cat(out, p, maxo, "PCI Passthrough (path B)\n");
    p = _pt_cat(out, p, maxo, "  iommu     : ");
    p = _pt_cat(out, p, maxo, IOMMU::IsSupported() ? "yes\n" : "no (passthrough disabled)\n");
    p = _pt_cat(out, p, maxo, "  devices   : ");
    p = _pt_dec(out, p, maxo, (uint32_t)Count());
    p = _pt_cat(out, p, maxo, "\n");
    for (int i = 0; i < entry_count; i++) {
        const PCIPassthroughEntry& e = entries[i];
        if (!e.in_use) continue;
        p = _pt_cat(out, p, maxo, "    [");
        p = _pt_dec(out, p, maxo, (uint32_t)i);
        p = _pt_cat(out, p, maxo, "] kind=");
        switch (e.kind) {
            case PT_KIND_NVIDIA_GPU: p = _pt_cat(out, p, maxo, "nvidia"); break;
            case PT_KIND_AMD_GPU:    p = _pt_cat(out, p, maxo, "amd"); break;
            default:                 p = _pt_cat(out, p, maxo, "generic"); break;
        }
        p = _pt_cat(out, p, maxo, "  host=");
        p = _pt_hex(out, p, maxo, e.host_bus);
        p = _pt_cat(out, p, maxo, ":");
        p = _pt_hex(out, p, maxo, e.host_dev);
        p = _pt_cat(out, p, maxo, ".");
        p = _pt_hex(out, p, maxo, e.host_func);
        p = _pt_cat(out, p, maxo, "  guest_slot=");
        p = _pt_dec(out, p, maxo, e.guest_slot);
        p = _pt_cat(out, p, maxo, "  bar0=0x");
        p = _pt_hex(out, p, maxo, e.bar_host_phys[0]);
        p = _pt_cat(out, p, maxo, "\n");
    }
    if (p < maxo) out[p] = 0;
}
