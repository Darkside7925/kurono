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
#include "../kernel/pci.h"
#include "../kernel/kimage.h"
#include "../kernel/kld_iommu.h"
#include "vdevices.h"

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

// ====================================================== generic, kurono guest
// a reserved host pci function handed to a `vm kurono` guest. the guest sees
// the real ids and bars on vpci; its bar accesses go straight to the device
// through ept (uncached), its dma goes through a vt-d domain whose iova is
// the guest-physical address, and the device's INTx is polled from the vm
// tick (host side INTx stays disabled). (satoru)
namespace {
    struct PtRes { uint8_t bus, dev, fn; };
    PtRes g_res[MAX_PT_DEVICES];
    int   g_nres = 0;
    const uint64_t PT_AS = 0x4B56504D00000001ull;   // the vt-d domain key (satoru)
    bool  g_dom = false;

    int hexv(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }
    void logbdf(uint8_t b, uint8_t d, uint8_t f) {
        const char* hx = "0123456789abcdef";
        char t[8] = { hx[b >> 4], hx[b & 15], ':', hx[(d >> 4) & 1], hx[d & 15], '.', (char)('0' + (f & 7)), 0 };
        SerialLogger::Log(t);
    }
    // the owner's hard list plus whole classes that must never leave the host (satoru)
    const char* refused(uint8_t b, uint8_t d, uint8_t f) {
        if (b == 0x02 && d == 0x00 && f == 0) return "02:00.0 is the laptop nvidia gpu";
        if (b == 0x81 && d == 0x00 && f == 0) return "81:00.0 is the laptop ethernet";
        if (b == 0x80 && d == 0x14 && f == 0) return "80:14.0 is the laptop usb controller";
        uint32_t id = PCI::Read32(b, d, f, 0);
        if ((id & 0xFFFF) == 0xFFFF) return "no device there";
        if ((id & 0xFFFF) == 0x10DE) return "nvidia devices stay on the host";
        uint8_t cls = (uint8_t)(PCI::Read32(b, d, f, 8) >> 24);
        if (cls == 0x01) return "storage controllers stay on the host";
        if (cls == 0x03) return "display controllers stay on the host";
        if (cls == 0x06) return "bridges cannot be passed through";
        return nullptr;
    }
    void cmd_write(uint8_t b, uint8_t d, uint8_t f, uint16_t cmd) {
        // the status half is write-1-to-clear: writing 0 there changes nothing (satoru)
        PCI::Write32(b, d, f, 0x04, (uint32_t)cmd);
    }

    // every guest ram page -> its host frame, coalesced into runs (satoru)
    bool map_guest_ram() {
        uint64_t pages = 0, runs = 0;
        uint64_t run_iova = 0, run_pa = 0; uint32_t run_n = 0;
        auto flush = [&]() -> bool {
            if (!run_n) return true;
            bool ok = KldIommu::MapIova(PT_AS, run_iova, run_pa, run_n);
            pages += run_n; runs++; run_n = 0;
            return ok;
        };
        auto add = [&](uint64_t iova, uint64_t pa) -> bool {
            if (run_n && iova == run_iova + (uint64_t)run_n * 4096 && pa == run_pa + (uint64_t)run_n * 4096 && run_n < 0x10000) {
                run_n++; return true;
            }
            if (!flush()) return false;
            run_iova = iova; run_pa = pa; run_n = 1;
            return true;
        };
        uint8_t* low = GuestMemoryManager::GetLowRAM();
        uint32_t lsz = GuestMemoryManager::GetLowRAMSize();
        for (uint32_t o = 0; low && o < lsz; o += 4096)
            if (!add(o, KernelPhysOf(low + o))) return false;
        for (uint32_t ci = 0; ci < GuestMemoryManager::GetHighChunkCount(); ci++) {
            uint64_t gpa = 0; uint8_t* h = nullptr; uint32_t sz = 0;
            if (!GuestMemoryManager::GetHighChunk(ci, &gpa, &h, &sz)) break;
            if (!h && !GuestMemoryManager::IsSplit(ci)) {
                // lazy chunk: back it now, a device may write it before the cpu does (satoru)
                uint64_t cg = 0; uint32_t cs = 0;
                if (!GuestMemoryManager::EnsureHighChunk(gpa, &cg, &h, &cs)) return false;
            }
            for (uint32_t o = 0; o < sz; o += 4096) {
                uint8_t* hp = GuestMemoryManager::GuestPhysToHost(gpa + o);
                if (!hp) return false;
                if (!add(gpa + o, KernelPhysOf(hp))) return false;
            }
        }
        if (!flush()) return false;
        KldIommu::FlushDomain(PT_AS);
        SerialLogger::Log("[vmpt] vt-d domain maps ");
        SerialLogger::LogDec((int)(pages / 256));
        SerialLogger::Log(" MB of guest ram in ");
        SerialLogger::LogDec((int)runs);
        SerialLogger::Log(" runs\r\n");
        return true;
    }

    void pt_cfg_notify(VPCIDevice* dev, uint8_t off, uint8_t size, uint32_t) {
        // only the command register reaches the device: memory + bus master
        // follow the guest, INTx disable stays set (satoru)
        if (!(off <= 4 && off + size > 4)) return;
        const PCIPassthroughEntry* e = nullptr;
        for (int i = 0; i < MAX_PT_DEVICES; i++) {
            const PCIPassthroughEntry* x = PCIPassthrough::Get(i);
            if (x && x->in_use && x->guest_slot == dev->dev) { e = x; break; }
        }
        if (!e) return;
        uint16_t g = (uint16_t)dev->cfg[4] | ((uint16_t)dev->cfg[5] << 8);
        cmd_write(e->host_bus, e->host_dev, e->host_func, (uint16_t)((g & 0x0006) | 0x0400));
    }
}

void PCIPassthrough::ParseReserveList(const char* s) {
    // BB:DD.F[,BB:DD.F...] in hex (satoru)
    while (s && *s && g_nres < MAX_PT_DEVICES) {
        int v[3] = {0, 0, 0}, k = 0, digits = 0;
        while (*s && *s != ',' && *s != ' ') {
            int h = hexv(*s);
            if (h >= 0) { v[k] = v[k] * 16 + h; digits++; }
            else if ((*s == ':' || *s == '.') && k < 2) k++;
            s++;
        }
        if (k == 2 && digits) {
            const char* why = refused((uint8_t)v[0], (uint8_t)v[1], (uint8_t)v[2]);
            SerialLogger::Log("[vmpt] reserve ");
            logbdf((uint8_t)v[0], (uint8_t)v[1], (uint8_t)v[2]);
            if (why) { SerialLogger::Log(" REFUSED: "); SerialLogger::Log(why); SerialLogger::Log("\r\n"); }
            else {
                SerialLogger::Log(" for vm guests (host drivers skip it)\r\n");
                g_res[g_nres++] = { (uint8_t)v[0], (uint8_t)v[1], (uint8_t)v[2] };
            }
        }
        while (*s == ',' || *s == ' ') s++;
    }
}

bool PCIPassthrough::IsReserved(uint8_t bus, uint8_t dev, uint8_t func) {
    for (int i = 0; i < g_nres; i++)
        if (g_res[i].bus == bus && g_res[i].dev == dev && g_res[i].fn == func) return true;
    return false;
}

int PCIPassthrough::ReservedCount() { return g_nres; }

int PCIPassthrough::AttachReserved(void* ept) {
    if (!g_nres || !ept) return 0;
    int attached = 0;
    for (int r = 0; r < g_nres && entry_count < MAX_PT_DEVICES; r++) {
        uint8_t b = g_res[r].bus, d = g_res[r].dev, f = g_res[r].fn;
        const char* why = refused(b, d, f);
        if (why) { SerialLogger::Log("[vmpt] refused: "); SerialLogger::Log(why); SerialLogger::Log("\r\n"); continue; }
        uint32_t bdf = ((uint32_t)b << 16) | ((uint32_t)d << 8) | f;
        // no vt-d, no passthrough: an untranslated device would dma into
        // host memory at the guest's addresses (satoru)
        if (!KldIommu::EnsureOn(bdf) || !KldIommu::Attach(PT_AS, "vm-pt", bdf)) {
            SerialLogger::Log("[vmpt] ");
            logbdf(b, d, f);
            SerialLogger::Log(" refused: vt-d is not available for it\r\n");
            continue;
        }
        if (!g_dom) {
            if (!map_guest_ram()) {
                SerialLogger::Log("[vmpt] guest ram map failed, releasing\r\n");
                KldIommu::ReleaseContainer(PT_AS);
                return attached;
            }
            g_dom = true;
        }
        // quiet the device while its bars are sized (satoru)
        uint16_t cmd0 = PCI::Read16(b, d, f, 0x04);
        cmd_write(b, d, f, (uint16_t)((cmd0 & ~0x0007u) | 0x0400));
        PCIPassthroughEntry* e = &entries[entry_count];
        *e = {};
        e->kind = PT_KIND_GENERIC_PCI;
        e->host_bus = b; e->host_dev = d; e->host_func = f;
        e->domain_id = 0;
        VPCIDevice v{};
        v.name = "vm-pt";
        for (int o = 0; o < 0x10; o += 4) VPCI::Cfg32(&v, (uint8_t)o, PCI::Read32(b, d, f, (uint8_t)o));
        VPCI::Cfg32(&v, 0x04, 0);                         // command + status start clear (satoru)
        v.cfg[PCI_HEADER_TYPE] &= 0x7F;                   // one function on our bus (satoru)
        VPCI::Cfg32(&v, 0x2C, PCI::Read32(b, d, f, 0x2C));
        v.cfg[PCI_CAP_PTR] = 0;                           // no msi/pm caps: INTx only (satoru)
        v.irq_line = 11;
        for (int i = 0; i < 6; i++) {
            uint8_t o = (uint8_t)(0x10 + i * 4);
            uint32_t lo = PCI::Read32(b, d, f, o);
            if (lo & 1) continue;                         // io bars are not forwarded (satoru)
            bool is64 = ((lo >> 1) & 3) == 2;
            PCI::Write32(b, d, f, o, 0xFFFFFFFFu);
            uint32_t mlo = PCI::Read32(b, d, f, o);
            PCI::Write32(b, d, f, o, lo);
            uint64_t mask = (uint64_t)(mlo & ~0xFu);
            uint64_t base = lo & ~0xFu;
            if (is64 && i < 5) {
                uint32_t hi = PCI::Read32(b, d, f, (uint8_t)(o + 4));
                PCI::Write32(b, d, f, (uint8_t)(o + 4), 0xFFFFFFFFu);
                uint32_t mhi = PCI::Read32(b, d, f, (uint8_t)(o + 4));
                PCI::Write32(b, d, f, (uint8_t)(o + 4), hi);
                mask |= (uint64_t)mhi << 32;
                base |= (uint64_t)hi << 32;
            } else {
                mask |= 0xFFFFFFFF00000000ull;
            }
            uint64_t size = mask ? (~mask + 1) : 0;
            if (size && size < 4096) size = 4096;         // ept maps whole pages (satoru)
            if (size && base && size <= 0x4000000ull) {
                v.bars[i].size = (uint32_t)size;
                v.bars[i].is_mmio = true;
                v.bars[i].is_64bit = is64;
                v.bars[i].prefetch = (lo & 8) != 0;
                e->bar_host_phys[i] = base;
                e->bar_size[i] = (uint32_t)size;
            }
            if (is64) i++;
        }
        v.cfg_notify = pt_cfg_notify;
        int slot = VPCI::RegisterDevice(&v);
        if (slot < 0) { SerialLogger::Log("[vmpt] vpci is full\r\n"); continue; }
        e->guest_slot = (uint8_t)slot;
        e->guest_irq = 11;
        e->in_use = true;
        entry_count++;
        VPCIDevice* c = VPCI::GetDevice(slot);
        for (int i = 0; c && i < 6; i++) {
            if (!e->bar_size[i]) continue;
            e->bar_guest_phys[i] = c->bars[i].base;
            EPTManager::MapMMIO((EPT_PML4*)ept, c->bars[i].base, e->bar_host_phys[i], e->bar_size[i]);
            SerialLogger::Log("[vmpt]   bar");
            SerialLogger::LogDec(i);
            SerialLogger::Log(" host 0x"); SerialLogger::LogHex64(e->bar_host_phys[i]);
            SerialLogger::Log(" -> guest 0x"); SerialLogger::LogHex64(c->bars[i].base);
            SerialLogger::Log(" size 0x"); SerialLogger::LogHex(e->bar_size[i]);
            SerialLogger::Log("\r\n");
        }
        SerialLogger::Log("[vmpt] ");
        logbdf(b, d, f);
        SerialLogger::Log(" -> guest ");
        logbdf(0, (uint8_t)slot, 0);
        SerialLogger::Log(" (irq 11, dma through vt-d)\r\n");
        attached++;
    }
    return attached;
}

void PCIPassthrough::DetachGuest() {
    bool any = false;
    for (int i = 0; i < entry_count; i++) {
        PCIPassthroughEntry& e = entries[i];
        if (!e.in_use || e.kind != PT_KIND_GENERIC_PCI) continue;
        // bus master off first: no dma into guest frames that are about to be freed (satoru)
        cmd_write(e.host_bus, e.host_dev, e.host_func, 0x0400);
        (void)PCI::Read32(e.host_bus, e.host_dev, e.host_func, 0x04);
        e.in_use = false;
        any = true;
    }
    if (g_dom || any) {
        KldIommu::ReleaseContainer(PT_AS);
        g_dom = false;
        SerialLogger::Log("[vmpt] passthrough devices detached\r\n");
    }
}

void PCIPassthrough::TickIntx() {
    for (int i = 0; i < entry_count; i++) {
        PCIPassthroughEntry& e = entries[i];
        if (!e.in_use || e.kind != PT_KIND_GENERIC_PCI) continue;
        // status bit 3 = the function asserts INTx, also while INTx is disabled (satoru)
        if (!(PCI::Read16(e.host_bus, e.host_dev, e.host_func, 0x06) & 0x08)) continue;
        if (e.guest_irq >= 8) {
            VirtualDevices::GetSlavePIC().SetIRQ(e.guest_irq - 8, true);
            VirtualDevices::GetSlavePIC().SetIRQ(e.guest_irq - 8, false);
        } else {
            VirtualDevices::GetMasterPIC().SetIRQ(e.guest_irq, true);
            VirtualDevices::GetMasterPIC().SetIRQ(e.guest_irq, false);
        }
    }
}
