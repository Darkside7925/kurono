//  KLD broker implementation - see kld_broker.h for the design and the honest
//  limits of the isolation it provides.
//
//  end (satoru)

#include "kld_broker.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "../drivers/serial.h"
#include "../proc/scheduler.h"
#include "../proc/smp.h"
#include "../security/admin.h"   // kld broker requires administrator approval (satoru)
#include "../proc/spinlock.h"
#include "multiboot.h"
#include "../hal/hal.h"
#include "../proc/smp.h"

// immediate futex wake by physical key, exported by the futex layer
// (linux_syscall.cpp, next to LinuxSyscallPollWakeInet). safe from isr context:
// it only takes irq-save spinlocks. (satoru)
extern int LinuxSyscallFutexWakePhys(uint64_t phys_key, int max);

namespace KldBroker {

namespace {

Spinlock g_kldb_lock;

// isr-side immediate wake of the container's irq thread. kurono.kld.irqwake=0
// turns it off, leaving only the futex repoll backstop (8 ms). (satoru)
bool g_irq_wake = true;

// ── grant table ─────────────────────────────────────────────────────────────
// one entry per (container address space, device) pair. small and fixed: a
// handful of driver containers is the realistic ceiling, and a fixed table
// means no allocation on a path an untrusted process can drive. (satoru)
constexpr int MAX_GRANTS = 16;

struct Grant {
    bool     used;
    uint64_t as;         // container address space = the grant key (satoru)
    int      pid;        // the thread that asked, for the log only (satoru)
    uint32_t bdf;
    uint32_t cfg_reads;
    uint32_t cfg_writes;
    uint32_t cfg_refused;
};

Grant g_grants[MAX_GRANTS];
uint32_t g_find_calls = 0;

// ── per-container resources ─────────────────────────────────────────────────
constexpr int MAX_CONTAINERS = 4;
constexpr int MAX_DMA        = 256;
constexpr int MAX_MAPS       = 16;

struct DmaRec { bool used; uint64_t va; uint64_t phys; uint32_t pages; };
struct MapRec { bool used; uint64_t va; uint32_t pages; };

struct Container {
    bool     used;
    uint64_t as;
    int      pid;
    uint64_t irq_page_phys;   // doorbell page (broker-owned frame) (satoru)
    uint64_t irq_page_va;
    uint64_t net_phys;        // shared packet rings (satoru)
    uint64_t net_va;
    DmaRec   dma[MAX_DMA];
    MapRec   maps[MAX_MAPS];
    uint32_t dma_pages;
};

Container g_cont[MAX_CONTAINERS];

// ── msi vectors ─────────────────────────────────────────────────────────────
struct MsiRec {
    volatile bool used;
    bool     msix;                      // msi-x table entry rather than msi cap (satoru)
    uint32_t msix_index;
    uint64_t as;
    uint32_t bdf;
    uint8_t  cap;
    uint64_t counter_phys;              // u32 in the container's doorbell page (satoru)
    volatile uint32_t* counter;         // same, through the identity map (satoru)
    volatile uint32_t fired;
};

MsiRec g_msi[MSI_VECTOR_COUNT];
volatile uint32_t g_vec_hits[MSI_VECTOR_COUNT];   // every delivery, used or not (satoru)

bool bdf_valid(uint32_t bdf) {
    // bus/dev/func must be in range and nothing may be set above bit 23. (satoru)
    if (bdf & 0xFF000000u) return false;
    uint8_t dev  = (uint8_t)((bdf >> 8) & 0xFF);
    uint8_t func = (uint8_t)(bdf & 0xFF);
    return dev < 32 && func < 8;
}

inline uint8_t bdf_bus(uint32_t bdf) { return (uint8_t)((bdf >> 16) & 0xFF); }
inline uint8_t bdf_dev(uint32_t bdf) { return (uint8_t)((bdf >> 8) & 0xFF); }
inline uint8_t bdf_fn(uint32_t bdf)  { return (uint8_t)(bdf & 0xFF); }

Grant* find_grant(uint64_t as, uint32_t bdf) {
    for (int i = 0; i < MAX_GRANTS; i++)
        if (g_grants[i].used && g_grants[i].as == as && g_grants[i].bdf == bdf)
            return &g_grants[i];
    return nullptr;
}

// a device may be granted to ONE container at a time: two drivers fighting
// over a nic is a bug class we refuse rather than debug. (satoru)
bool granted_elsewhere(uint64_t as, uint32_t bdf) {
    for (int i = 0; i < MAX_GRANTS; i++)
        if (g_grants[i].used && g_grants[i].bdf == bdf && g_grants[i].as != as)
            return true;
    return false;
}

bool add_grant(uint64_t as, int pid, uint32_t bdf) {
    if (find_grant(as, bdf)) return true;
    for (int i = 0; i < MAX_GRANTS; i++) {
        if (!g_grants[i].used) {
            g_grants[i] = {};
            g_grants[i].used = true;
            g_grants[i].as = as;
            g_grants[i].pid = pid;
            g_grants[i].bdf = bdf;
            return true;
        }
    }
    return false;
}

Container* find_container(uint64_t as, bool create, int pid) {
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (g_cont[i].used && g_cont[i].as == as) return &g_cont[i];
    if (!create) return nullptr;
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (!g_cont[i].used) {
            g_cont[i] = {};
            g_cont[i].used = true;
            g_cont[i].as = as;
            g_cont[i].pid = pid;
            return &g_cont[i];
        }
    }
    return nullptr;
}

void log_bdf(const char* pfx, uint32_t bdf) {
    SerialLogger::Log(pfx);
    SerialLogger::LogDec((int)bdf_bus(bdf));
    SerialLogger::Log(":");
    SerialLogger::LogDec((int)bdf_dev(bdf));
    SerialLogger::Log(".");
    SerialLogger::LogDec((int)bdf_fn(bdf));
}

// ── config helpers (cf8/cfc is a two-step protocol, so serialize) ───────────
Spinlock g_cfg_lock;

uint32_t cfg_r32(uint32_t bdf, uint32_t off) {
    SpinLockGuard g(g_cfg_lock);
    return PCI::Read32(bdf_bus(bdf), bdf_dev(bdf), bdf_fn(bdf), (uint8_t)(off & 0xFC));
}

void cfg_w32(uint32_t bdf, uint32_t off, uint32_t v) {
    SpinLockGuard g(g_cfg_lock);
    PCI::Write32(bdf_bus(bdf), bdf_dev(bdf), bdf_fn(bdf), (uint8_t)(off & 0xFC), v);
}

uint32_t cfg_read(uint32_t bdf, uint32_t off, uint32_t width) {
    uint32_t d = cfg_r32(bdf, off);
    uint32_t sh = (off & 3u) * 8u;
    if (width == 1) return (d >> sh) & 0xFFu;
    if (width == 2) return (d >> sh) & 0xFFFFu;
    return d;
}

// config writes are byte/word granular in the driver but the accessor is
// dword-wide, so read-modify-write the containing dword rather than clobbering
// neighbouring registers. (satoru)
void cfg_write(uint32_t bdf, uint32_t off, uint32_t width, uint32_t val) {
    if (width == 4) { cfg_w32(bdf, off, val); return; }
    SpinLockGuard g(g_cfg_lock);
    uint8_t b = bdf_bus(bdf), d = bdf_dev(bdf), f = bdf_fn(bdf);
    uint32_t cur = PCI::Read32(b, d, f, (uint8_t)(off & 0xFC));
    uint32_t sh = (off & 3u) * 8u;
    uint32_t mask = (width == 1) ? 0xFFu : 0xFFFFu;
    cur &= ~(mask << sh);
    cur |= (val & mask) << sh;
    PCI::Write32(b, d, f, (uint8_t)(off & 0xFC), cur);
}

// walk the standard capability list. returns the offset or 0. (satoru)
uint8_t find_cap(uint32_t bdf, uint8_t id) {
    if (!(cfg_read(bdf, 0x06, 2) & 0x10)) return 0;
    uint8_t p = (uint8_t)(cfg_read(bdf, 0x34, 1) & 0xFC);
    for (int guard = 0; p >= 0x40 && guard < 48; guard++) {
        if (cfg_read(bdf, p, 1) == id) return p;
        p = (uint8_t)(cfg_read(bdf, p + 1u, 1) & 0xFC);
    }
    return 0;
}

// does [off, off+width) overlap [lo, hi)? (satoru)
inline bool overlaps(uint32_t off, uint32_t width, uint32_t lo, uint32_t hi) {
    return off < hi && off + width > lo;
}

// writes the broker refuses: BARs + expansion rom (decode belongs to the
// firmware/kernel allocation) and the MSI / MSI-X capability BODIES (interrupt
// routing belongs to the broker; a container that could aim MSI at an
// arbitrary lapic vector could inject interrupts into the kernel). the cap
// header byte pair (id/next) is read-only in hardware anyway. (satoru)
bool cfg_write_refused(uint32_t bdf, uint32_t off, uint32_t width) {
    if (overlaps(off, width, 0x10, 0x28)) return true;
    if (overlaps(off, width, 0x30, 0x34)) return true;
    uint8_t msi = find_cap(bdf, 0x05);
    if (msi && overlaps(off, width, msi, msi + 0x18u)) return true;
    uint8_t msix = find_cap(bdf, 0x11);
    if (msix && overlaps(off, width, msix, msix + 0x0Cu)) return true;
    return false;
}

// ── bar decoding ────────────────────────────────────────────────────────────
// returns the physical base of a memory bar (0 for io / unused / upper half).
uint64_t bar_base(uint32_t bdf, uint32_t idx, bool* is64) {
    uint32_t lo = cfg_r32(bdf, 0x10 + idx * 4);
    *is64 = false;
    if (lo & 1) return 0;                       // io bar (satoru)
    uint64_t base = lo & ~0xFull;
    if (((lo >> 1) & 3) == 2) {
        if (idx >= 5) return 0;
        *is64 = true;
        base |= (uint64_t)cfg_r32(bdf, 0x10 + (idx + 1) * 4) << 32;
    }
    return base;
}

// size a bar the standard way: decode OFF, write all-ones, read back, restore.
// done here (not in the container) because the container may not write bars.
// (satoru)
uint64_t bar_size(uint32_t bdf, uint32_t idx) {
    uint32_t lo = cfg_r32(bdf, 0x10 + idx * 4);
    if (lo == 0) return 0;
    uint32_t cmd = cfg_read(bdf, 0x04, 2);
    cfg_write(bdf, 0x04, 2, cmd & ~0x3u);
    bool io = (lo & 1) != 0;
    bool b64 = !io && (((lo >> 1) & 3) == 2) && idx < 5;
    uint32_t hi = b64 ? cfg_r32(bdf, 0x10 + (idx + 1) * 4) : 0;
    cfg_w32(bdf, 0x10 + idx * 4, 0xFFFFFFFFu);
    uint32_t mlo = cfg_r32(bdf, 0x10 + idx * 4);
    uint32_t mhi = 0xFFFFFFFFu;
    if (b64) {
        cfg_w32(bdf, 0x10 + (idx + 1) * 4, 0xFFFFFFFFu);
        mhi = cfg_r32(bdf, 0x10 + (idx + 1) * 4);
        cfg_w32(bdf, 0x10 + (idx + 1) * 4, hi);
    }
    cfg_w32(bdf, 0x10 + idx * 4, lo);
    cfg_write(bdf, 0x04, 2, cmd);
    uint64_t mask;
    if (io) mask = (uint64_t)(mlo & ~0x3u) | 0xFFFF0000ull;
    else    mask = ((uint64_t)mhi << 32) | (mlo & ~0xFull);
    if (!b64 && !io) mask |= 0xFFFFFFFF00000000ull;
    uint64_t sz = (~mask) + 1;
    return sz;
}

// ── user va checks ──────────────────────────────────────────────────────────
// a broker mapping target must be page aligned, wholly inside the user half
// and NOT currently mapped: the container reserves it with an anonymous mmap
// (demand-zero, so no frame exists yet) and we fill it. refusing a mapped page
// means we never displace (and leak or double-free) a frame. (satoru)
bool va_range_free(uint64_t as, uint64_t va, uint64_t pages) {
    if (va & 0xFFFull) return false;
    if (pages == 0 || pages > (1ull << 20)) return false;
    if (va < 0x10000ull) return false;
    uint64_t end = va + pages * PAGE_SIZE;
    if (end < va || end > USER_SPACE_TOP + 1) return false;
    for (uint64_t i = 0; i < pages; i++)
        if (KernelVMM::QueryMappingInAddressSpace(as, va + i * PAGE_SIZE)) return false;
    return true;
}

bool map_pages(uint64_t as, uint64_t va, uint64_t phys, uint64_t pages, uint64_t flags) {
    for (uint64_t i = 0; i < pages; i++) {
        if (!KernelVMM::MapPageInAddressSpace(as, va + i * PAGE_SIZE, phys + i * PAGE_SIZE, flags)) {
            for (uint64_t j = 0; j < i; j++)
                KernelVMM::UnmapPageInAddressSpace(as, va + j * PAGE_SIZE, false);
            return false;
        }
    }
    return true;
}

void unmap_pages(uint64_t as, uint64_t va, uint64_t pages) {
    for (uint64_t i = 0; i < pages; i++)
        KernelVMM::UnmapPageInAddressSpace(as, va + i * PAGE_SIZE, false);
    // other cores may run threads of this container. (satoru)
    if (SMP::OnlineCount() > 1) SMP::BroadcastTlbFlush();
}

// broker-owned frames that a container mapping also references: the mapping
// holds one ref (dropped by the vmm teardown or by our explicit unmap), the
// broker holds the allocation ref. so whichever of "container dies" and
// "broker releases" happens last actually frees the frame. (satoru)
uint64_t alloc_owned(uint64_t pages, uint32_t phys_bits) {
    uint64_t phys = PMM::AllocContiguous(pages);
    if (!phys) return 0;
    uint64_t limit = (phys_bits >= 52) ? ~0ull : (1ull << phys_bits);
    // the kernel identity map covers 0..16 GiB; never hand out a frame the
    // broker cannot reach itself. (satoru)
    uint64_t ident = 16ull << 30;
    if (phys + pages * PAGE_SIZE > limit || phys + pages * PAGE_SIZE > ident) {
        PMM::FreeContiguous(phys, pages);
        return 0;
    }
    for (uint64_t i = 0; i < pages; i++) PMM::RetainFrame(phys + i * PAGE_SIZE);
    return phys;
}

// drop the broker's allocation ref (the mapping's ref is dropped separately).
void free_owned(uint64_t phys, uint64_t pages) {
    for (uint64_t i = 0; i < pages; i++) PMM::FreeFrame(phys + i * PAGE_SIZE);
}

// ── ops ─────────────────────────────────────────────────────────────────────

// ── the device cache ────────────────────────────────────────────────────────
// A bus scan MUST be cached. The first version scanned 256x32x8 on every
// PCI_FIND, and a driver with a 115-entry id_table (e1000e) therefore drove
// ~7.5 MILLION config reads, each two port-I/O cycles - it never finished.
// Scan once into this table, then FIND is a linear search over a handful of
// entries. (satoru)
constexpr int MAX_DEVS = 64;

struct DevEntry {
    uint32_t bdf;
    uint16_t vendor;
    uint16_t device;
};

DevEntry g_devs[MAX_DEVS];
int  g_dev_count = 0;
bool g_scanned = false;

// buses 0..7 covers qemu and the primary buses of real machines. NOT a full
// recursive bridge walk - a device behind a deeper bridge is invisible, which is
// a known limit rather than a silent one. (satoru)
constexpr uint32_t SCAN_BUS_LIMIT = 8;

void scan_bus_once() {
    if (g_scanned) return;
    g_scanned = true;
    for (uint32_t bus = 0; bus < SCAN_BUS_LIMIT; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint16_t v = PCI::GetVendor((uint8_t)bus, dev, func);
                if (v == 0xFFFF || v == 0) { if (func == 0) break; continue; }
                if (g_dev_count >= MAX_DEVS) goto done;
                g_devs[g_dev_count].bdf    = PackBdf((uint8_t)bus, dev, func);
                g_devs[g_dev_count].vendor = v;
                g_devs[g_dev_count].device = PCI::GetDevice((uint8_t)bus, dev, func);
                g_dev_count++;
                // a non-multifunction device (header type bit 7 clear) has no
                // func 1..7; skipping them cuts the scan 8x. (satoru)
                if (func == 0) {
                    uint8_t hdr = PCI::Read8((uint8_t)bus, dev, 0, 0x0E);
                    if (!(hdr & 0x80)) break;
                }
            }
        }
    }
done:
    SerialLogger::Log("[kldb] pci scan cached: ");
    SerialLogger::LogDec(g_dev_count);
    SerialLogger::Log(" devices on buses 0-");
    SerialLogger::LogDec((int)SCAN_BUS_LIMIT - 1);
    SerialLogger::Log("\r\n");
}

// match a vendor/device against the cache, skipping the first `skip` hits so a
// container can enumerate multiple identical NICs. a match GRANTS it. (satoru)
int64_t op_pci_find(uint64_t as, int pid, uint32_t vendor, uint32_t device, uint32_t skip) {
    g_find_calls++;
    if (vendor == 0 || vendor == 0xFFFF) return KLD_EINVAL;
    scan_bus_once();

    uint32_t seen = 0;
    for (int i = 0; i < g_dev_count; i++) {
        if (g_devs[i].vendor != (uint16_t)vendor) continue;
        if (g_devs[i].device != (uint16_t)device) continue;
        if (seen++ < skip) continue;
        if (granted_elsewhere(as, g_devs[i].bdf)) return KLD_EBUSY;
        if (!add_grant(as, pid, g_devs[i].bdf)) return KLD_ENOSPC;
        find_container(as, true, pid);
        log_bdf("[kldb] granted ", g_devs[i].bdf);
        SerialLogger::Log(" to pid ");
        SerialLogger::LogDec(pid);
        SerialLogger::Log(" (vendor/device matched)\r\n");
        return (int64_t)g_devs[i].bdf;
    }
    return KLD_ENODEV;
}

int64_t op_cfg_read(uint64_t as, uint32_t bdf, uint32_t off, uint32_t width) {
    if (!bdf_valid(bdf)) return KLD_EINVAL;
    // the cf8 mechanism reaches 0..0xFF only. extended config (0x100..0xFFF)
    // needs ECAM from the MCFG table; refusing beats silently reading offset
    // (off & 0xFF), which is what the old code did. (satoru)
    if (off > 0xFF) return KLD_EINVAL;
    if (width != 1 && width != 2 && width != 4) return KLD_EINVAL;
    if (off & (width - 1)) return KLD_EINVAL;
    Grant* g = find_grant(as, bdf);
    if (!g) return KLD_EPERM;
    g->cfg_reads++;
    return (int64_t)cfg_read(bdf, off, width);
}

int64_t op_cfg_write(uint64_t as, uint32_t bdf, uint32_t off, uint32_t width, uint32_t val) {
    if (!bdf_valid(bdf)) return KLD_EINVAL;
    if (off > 0xFF) return KLD_EINVAL;
    if (width != 1 && width != 2 && width != 4) return KLD_EINVAL;
    if (off & (width - 1)) return KLD_EINVAL;
    Grant* g = find_grant(as, bdf);
    if (!g) return KLD_EPERM;
    if (cfg_write_refused(bdf, off, width)) {
        g->cfg_refused++;
        if (g->cfg_refused <= 4) {
            log_bdf("[kldb] refused config write ", bdf);
            SerialLogger::Log(" off=0x");
            SerialLogger::LogHex(off);
            SerialLogger::Log(" (bar/msi owned by the broker)\r\n");
        }
        return KLD_EPERM;
    }
    g->cfg_writes++;
    cfg_write(bdf, off, width, val);
    return KLD_OK;
}

int64_t op_pci_bar(uint64_t as, uint32_t bdf, uint32_t idx) {
    if (!bdf_valid(bdf)) return KLD_EINVAL;
    if (idx > 5) return KLD_EINVAL;
    if (!find_grant(as, bdf)) return KLD_EPERM;
    return (int64_t)cfg_r32(bdf, 0x10 + idx * 4);
}

int64_t op_pci_bar_size(uint64_t as, uint32_t bdf, uint32_t idx) {
    if (!bdf_valid(bdf)) return KLD_EINVAL;
    if (idx > 5) return KLD_EINVAL;
    if (!find_grant(as, bdf)) return KLD_EPERM;
    return (int64_t)bar_size(bdf, idx);
}

int64_t op_pci_enable(uint64_t as, uint32_t bdf) {
    if (!bdf_valid(bdf)) return KLD_EINVAL;
    if (!find_grant(as, bdf)) return KLD_EPERM;
    // memory space + bus master. (satoru)
    uint32_t cmd = cfg_read(bdf, 0x04, 2);
    cfg_write(bdf, 0x04, 2, cmd | 0x6u);
    log_bdf("[kldb] enabled (mem+busmaster) ", bdf);
    SerialLogger::Log("\r\n");
    return KLD_OK;
}

int64_t op_pci_disable(uint64_t as, uint32_t bdf) {
    if (!bdf_valid(bdf)) return KLD_EINVAL;
    if (!find_grant(as, bdf)) return KLD_EPERM;
    uint32_t cmd = cfg_read(bdf, 0x04, 2);
    cfg_write(bdf, 0x04, 2, cmd & ~0x4u);
    return KLD_OK;
}

bool msix_overlaps(uint32_t bdf, uint64_t phys, uint64_t bytes);

int64_t op_map_bar(uint64_t as, uint32_t bdf, uint32_t idx, uint64_t va, uint64_t bytes) {
    if (!bdf_valid(bdf) || idx > 5) return KLD_EINVAL;
    if (!find_grant(as, bdf)) return KLD_EPERM;
    bool is64 = false;
    uint64_t base = bar_base(bdf, idx, &is64);
    if (!base) return KLD_ENODEV;
    uint64_t size = bar_size(bdf, idx);
    if (bytes == 0 || bytes > size) bytes = size;
    uint64_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    Container* c = find_container(as, true, -1);
    if (!c) return KLD_ENOSPC;
    MapRec* m = nullptr;
    for (int i = 0; i < MAX_MAPS; i++) if (!c->maps[i].used) { m = &c->maps[i]; break; }
    if (!m) return KLD_ENOSPC;
    if (!va_range_free(as, va, pages)) return KLD_EBUSY;
    // the msi-x table/pba belong to the broker. a bar that holds them is
    // refused as a whole (82574/igc keep them in a bar of their own). (satoru)
    if (msix_overlaps(bdf, base, pages * PAGE_SIZE)) {
        log_bdf("[kldb] refused MAP_BAR over the msi-x table of ", bdf);
        SerialLogger::Log("\r\n");
        return KLD_EPERM;
    }
    // pin the mmio frames: some of a pci hole sits INSIDE the pmm bitmap range
    // (marked used forever). a teardown that FreeFrame'd such a frame would put
    // device memory on the free list. two refs means a teardown only ever
    // decrements. nx as well: mmio is never executed. (satoru)
    for (uint64_t i = 0; i < pages; i++) {
        PMM::RetainFrame(base + i * PAGE_SIZE);
        PMM::RetainFrame(base + i * PAGE_SIZE);
    }
    uint64_t fl = PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_PCD | PTE_PWT | PTE_NX;
    if (!map_pages(as, va, base, pages, fl)) return KLD_ENOMEM;
    m->used = true; m->va = va; m->pages = (uint32_t)pages;
    log_bdf("[kldb] mapped bar of ", bdf);
    SerialLogger::Log(" idx="); SerialLogger::LogDec((int)idx);
    SerialLogger::Log(" phys=0x"); SerialLogger::LogHex64(base);
    SerialLogger::Log(" pages="); SerialLogger::LogDec((int)pages);
    SerialLogger::Log(" (uc,nx)\r\n");
    return KLD_OK;
}

int64_t op_dma_alloc(uint64_t as, int pid, uint64_t va, uint64_t pages, uint32_t bits) {
    if (bits < 24 || bits > 64) return KLD_EINVAL;
    if (pages == 0 || pages > 4096) return KLD_EINVAL;
    Container* c = find_container(as, false, pid);
    if (!c) return KLD_EPERM;   // only a container holding a grant may allocate (satoru)
    // per-container cap: 64 MiB of pinned frames. (satoru)
    if (c->dma_pages + pages > 16384) return KLD_ENOMEM;
    DmaRec* r = nullptr;
    for (int i = 0; i < MAX_DMA; i++) if (!c->dma[i].used) { r = &c->dma[i]; break; }
    if (!r) return KLD_ENOSPC;
    if (!va_range_free(as, va, pages)) return KLD_EBUSY;
    uint64_t phys = alloc_owned(pages, bits);
    if (!phys) return KLD_ENOMEM;
    // x86 dma is cache coherent: write-back is correct for coherent memory. (satoru)
    uint64_t fl = PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_NX;
    if (!map_pages(as, va, phys, pages, fl)) {
        // drop the retain and the allocation. (satoru)
        free_owned(phys, pages);
        free_owned(phys, pages);
        return KLD_ENOMEM;
    }
    r->used = true; r->va = va; r->phys = phys; r->pages = (uint32_t)pages;
    c->dma_pages += (uint32_t)pages;
    return (int64_t)phys;
}

int64_t op_dma_free(uint64_t as, uint64_t va) {
    Container* c = find_container(as, false, -1);
    if (!c) return KLD_EPERM;
    for (int i = 0; i < MAX_DMA; i++) {
        DmaRec* r = &c->dma[i];
        if (!r->used || r->va != va) continue;
        unmap_pages(as, r->va, r->pages);
        free_owned(r->phys, r->pages);   // the mapping's ref (satoru)
        free_owned(r->phys, r->pages);   // the broker's ref (satoru)
        c->dma_pages -= r->pages;
        r->used = false;
        return KLD_OK;
    }
    return KLD_EINVAL;
}

int64_t op_irq_page(uint64_t as, int pid, uint64_t va) {
    Container* c = find_container(as, false, pid);
    if (!c) return KLD_EPERM;
    if (c->irq_page_phys) return KLD_EBUSY;
    if (!va_range_free(as, va, 1)) return KLD_EBUSY;
    uint64_t phys = alloc_owned(1, 52);
    if (!phys) return KLD_ENOMEM;
    if (!map_pages(as, va, phys, 1, PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_NX)) {
        free_owned(phys, 1); free_owned(phys, 1);
        return KLD_ENOMEM;
    }
    c->irq_page_phys = phys;
    c->irq_page_va = va;
    return KLD_OK;
}

int64_t op_msi_alloc(uint64_t as, uint32_t bdf) {
    if (!bdf_valid(bdf)) return KLD_EINVAL;
    if (!find_grant(as, bdf)) return KLD_EPERM;
    Container* c = find_container(as, false, -1);
    if (!c || !c->irq_page_phys) return KLD_EINVAL;
    uint8_t cap = find_cap(bdf, 0x05);
    if (!cap) return KLD_ENODEV;
    int h = -1;
    {
        SpinLockGuard g(g_kldb_lock);
        for (int i = 0; i < (int)MSI_VECTOR_COUNT; i++) if (!g_msi[i].used) { h = i; break; }
        if (h < 0) return KLD_ENOSPC;
        MsiRec* m = &g_msi[h];
        m->as = as;
        m->bdf = bdf;
        m->cap = cap;
        m->msix = false;
        m->counter_phys = c->irq_page_phys + 4ull * (uint64_t)h;
        m->counter = (volatile uint32_t*)(uintptr_t)m->counter_phys;
        m->fired = 0;
        __atomic_store_n(&m->used, true, __ATOMIC_RELEASE);
    }
    // program: address = the bsp's lapic (physical destination, fixed delivery),
    // data = our vector, edge. one vector (mme = 0). intx off while msi is on,
    // like linux's pci_intx(dev, 0). (satoru)
    uint32_t vec = MSI_VECTOR_BASE + (uint32_t)h;
    PerCpu* bsp = SMP::ByIndex(0);
    uint32_t apic = bsp ? bsp->apic_id : 0;
    uint32_t ctrl = cfg_read(bdf, cap + 2u, 2);
    bool b64 = (ctrl & 0x80) != 0;
    cfg_write(bdf, cap + 2u, 2, ctrl & ~0x71u);           // disable while programming (satoru)
    cfg_w32(bdf, cap + 4u, 0xFEE00000u | ((apic & 0xFFu) << 12));
    if (b64) {
        cfg_w32(bdf, cap + 8u, 0);
        cfg_write(bdf, cap + 0xCu, 2, vec);
    } else {
        cfg_write(bdf, cap + 8u, 2, vec);
    }
    uint32_t cmd = cfg_read(bdf, 0x04, 2);
    cfg_write(bdf, 0x04, 2, cmd | 0x400u);                 // intx disable (satoru)
    cfg_write(bdf, cap + 2u, 2, (ctrl & ~0x70u) | 0x1u);   // msi enable (satoru)
    log_bdf("[kldb] msi ", bdf);
    SerialLogger::Log(" -> vector 0x"); SerialLogger::LogHex(vec);
    SerialLogger::Log(" apic "); SerialLogger::LogDec((int)apic);
    SerialLogger::Log(b64 ? " (64-bit)" : " (32-bit)");
    SerialLogger::Log(" handle "); SerialLogger::LogDec(h);
    SerialLogger::Log("\r\n");
    return h;
}

void msi_disable_locked(int h) {
    MsiRec* m = &g_msi[h];
    if (!m->used) return;
    uint32_t ctrl = cfg_read(m->bdf, m->cap + 2u, 2);
    if (m->msix) cfg_write(m->bdf, m->cap + 2u, 2, (ctrl | 0x4000u) & ~0x8000u);   // mask + disable (satoru)
    else         cfg_write(m->bdf, m->cap + 2u, 2, ctrl & ~0x1u);
    __atomic_store_n(&m->used, false, __ATOMIC_RELEASE);
}

int64_t op_msi_free(uint64_t as, uint32_t h) {
    if (h >= MSI_VECTOR_COUNT) return KLD_EINVAL;
    if (!g_msi[h].used || g_msi[h].as != as) return KLD_EPERM;
    msi_disable_locked((int)h);
    return KLD_OK;
}

// ── msi-x ───────────────────────────────────────────────────────────────────
// the table lives in a memory bar (bir + offset from the capability). the
// broker writes entries through the kernel identity map of that bar, never
// through the container, and MAP_BAR refuses to hand the table/pba pages to
// the container (see msix_overlaps). (satoru)
struct MsixInfo { uint8_t cap; uint32_t size; uint64_t table; uint64_t pba; uint32_t pba_bytes; };

bool msix_info(uint32_t bdf, MsixInfo* mi) {
    uint8_t cap = find_cap(bdf, 0x11);
    if (!cap) return false;
    uint32_t ctrl = cfg_read(bdf, cap + 2u, 2);
    uint32_t tbl = cfg_r32(bdf, cap + 4u);
    uint32_t pba = cfg_r32(bdf, cap + 8u);
    bool b64 = false;
    uint64_t tb = bar_base(bdf, tbl & 7u, &b64);
    uint64_t pb = bar_base(bdf, pba & 7u, &b64);
    if (!tb || !pb) return false;
    mi->cap = cap;
    mi->size = (ctrl & 0x7FFu) + 1u;
    mi->table = tb + (tbl & ~7u);
    mi->pba = pb + (pba & ~7u);
    mi->pba_bytes = ((mi->size + 63u) / 64u) * 8u;
    return true;
}

// does the physical page range [phys, phys+bytes) touch the msi-x table or pba? (satoru)
bool msix_overlaps(uint32_t bdf, uint64_t phys, uint64_t bytes) {
    MsixInfo mi;
    if (!msix_info(bdf, &mi)) return false;
    uint64_t t0 = mi.table & ~0xFFFull, t1 = (mi.table + mi.size * 16ull + 0xFFFull) & ~0xFFFull;
    uint64_t p0 = mi.pba & ~0xFFFull,   p1 = (mi.pba + mi.pba_bytes + 0xFFFull) & ~0xFFFull;
    uint64_t e = phys + bytes;
    return (phys < t1 && e > t0) || (phys < p1 && e > p0);
}

uint32_t g_msix_granted[MAX_GRANTS];   // per grant slot (satoru)

int64_t op_msix_alloc(uint64_t as, uint32_t bdf, uint32_t want) {
    if (!bdf_valid(bdf)) return KLD_EINVAL;
    Grant* gr = find_grant(as, bdf);
    if (!gr) return KLD_EPERM;
    Container* c = find_container(as, false, -1);
    if (!c || !c->irq_page_phys) return KLD_EINVAL;
    MsixInfo mi;
    if (!msix_info(bdf, &mi)) return KLD_ENODEV;
    if (want == 0) return KLD_EINVAL;
    if (want > mi.size) want = mi.size;
    int first = -1;
    uint32_t got = 0;
    {
        SpinLockGuard g(g_kldb_lock);
        // consecutive free handles so the container can index them. (satoru)
        for (int start = 0; start < (int)MSI_VECTOR_COUNT && first < 0; start++) {
            uint32_t run = 0;
            while (start + (int)run < (int)MSI_VECTOR_COUNT && run < want && !g_msi[start + run].used) run++;
            if (run == want || (run > 0 && start + (int)run == (int)MSI_VECTOR_COUNT)) {
                first = start; got = run;
            }
        }
        if (first < 0) return KLD_ENOSPC;
        for (uint32_t i = 0; i < got; i++) {
            MsiRec* m = &g_msi[first + i];
            m->as = as; m->bdf = bdf; m->cap = mi.cap;
            m->msix = true; m->msix_index = i;
            m->counter_phys = c->irq_page_phys + 4ull * (uint64_t)(first + i);
            m->counter = (volatile uint32_t*)(uintptr_t)m->counter_phys;
            m->fired = 0;
            __atomic_store_n(&m->used, true, __ATOMIC_RELEASE);
        }
    }
    PerCpu* bsp = SMP::ByIndex(0);
    uint32_t apic = bsp ? bsp->apic_id : 0;
    uint32_t ctrl = cfg_read(bdf, mi.cap + 2u, 2);
    // function-mask while programming, as the spec recommends. (satoru)
    cfg_write(bdf, mi.cap + 2u, 2, (ctrl | 0x4000u) & ~0x8000u);
    cfg_write(bdf, mi.cap + 2u, 2, ctrl | 0xC000u);
    for (uint32_t i = 0; i < mi.size; i++) {
        volatile uint32_t* e = (volatile uint32_t*)(uintptr_t)(mi.table + 16ull * i);
        if (i < got) {
            e[0] = 0xFEE00000u | ((apic & 0xFFu) << 12);
            e[1] = 0;
            e[2] = MSI_VECTOR_BASE + (uint32_t)first + i;
            e[3] = 0;                       // unmasked (satoru)
        } else {
            e[3] = 1;                       // masked (satoru)
        }
    }
    uint32_t cmd = cfg_read(bdf, 0x04, 2);
    cfg_write(bdf, 0x04, 2, cmd | 0x400u);                     // intx off (satoru)
    cfg_write(bdf, mi.cap + 2u, 2, (ctrl | 0x8000u) & ~0x4000u); // enable, unmask (satoru)
    for (int i = 0; i < MAX_GRANTS; i++) if (&g_grants[i] == gr) g_msix_granted[i] = got;
    log_bdf("[kldb] msi-x ", bdf);
    SerialLogger::Log(" -> "); SerialLogger::LogDec((int)got);
    SerialLogger::Log(" vectors from 0x"); SerialLogger::LogHex(MSI_VECTOR_BASE + (uint32_t)first);
    SerialLogger::Log(" (table size "); SerialLogger::LogDec((int)mi.size);
    SerialLogger::Log(", programmed by the broker)\r\n");
    return first;
}

int64_t op_msix_free(uint64_t as, uint32_t bdf) {
    if (!bdf_valid(bdf)) return KLD_EINVAL;
    if (!find_grant(as, bdf)) return KLD_EPERM;
    MsixInfo mi;
    if (!msix_info(bdf, &mi)) return KLD_ENODEV;
    uint32_t ctrl = cfg_read(bdf, mi.cap + 2u, 2);
    cfg_write(bdf, mi.cap + 2u, 2, (ctrl | 0x4000u) & ~0x8000u);
    for (uint32_t i = 0; i < mi.size; i++) ((volatile uint32_t*)(uintptr_t)(mi.table + 16ull * i))[3] = 1;
    for (int h = 0; h < (int)MSI_VECTOR_COUNT; h++)
        if (g_msi[h].used && g_msi[h].msix && g_msi[h].as == as && g_msi[h].bdf == bdf)
            __atomic_store_n(&g_msi[h].used, false, __ATOMIC_RELEASE);
    return KLD_OK;
}

int64_t op_msix_count(uint64_t as, uint32_t bdf) {
    for (int i = 0; i < MAX_GRANTS; i++)
        if (g_grants[i].used && g_grants[i].as == as && g_grants[i].bdf == bdf) return g_msix_granted[i];
    return KLD_EPERM;
}

int64_t op_net_attach(uint64_t as, int pid, uint64_t va, uint64_t mac_lo, uint64_t mac_hi) {
    Container* c = find_container(as, false, pid);
    if (!c) return KLD_EPERM;
    if (c->net_phys) return KLD_EBUSY;
    uint64_t pages = KldNet::KLD_NET_PAGES;
    if (!va_range_free(as, va, pages)) return KLD_EBUSY;
    uint64_t phys = alloc_owned(pages, 52);
    if (!phys) return KLD_ENOMEM;
    if (!map_pages(as, va, phys, pages, PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_NX)) {
        free_owned(phys, pages); free_owned(phys, pages);
        return KLD_ENOMEM;
    }
    uint8_t mac[6];
    for (int i = 0; i < 4; i++) mac[i] = (uint8_t)(mac_lo >> (8 * i));
    mac[4] = (uint8_t)mac_hi; mac[5] = (uint8_t)(mac_hi >> 8);
    int64_t r = KldNet::Attach(as, phys, mac);
    if (r < 0) {
        unmap_pages(as, va, pages);
        free_owned(phys, pages); free_owned(phys, pages);
        return r;
    }
    c->net_phys = phys;
    c->net_va = va;
    return KLD_OK;
}

void release_container(Container* c) {
    // 1) bus mastering off on every granted device: the device must stop
    //    writing into frames before any of them can be freed. (satoru)
    for (int i = 0; i < MAX_GRANTS; i++) {
        if (!g_grants[i].used || g_grants[i].as != c->as) continue;
        uint32_t bdf = g_grants[i].bdf;
        uint32_t cmd = cfg_read(bdf, 0x04, 2);
        cfg_write(bdf, 0x04, 2, cmd & ~0x4u);
    }
    // 2) msi off + vectors free. (satoru)
    for (int h = 0; h < (int)MSI_VECTOR_COUNT; h++)
        if (g_msi[h].used && g_msi[h].as == c->as) msi_disable_locked(h);
    // 3) the net backend stops reading the rings. (satoru)
    if (c->net_phys) {
        KldNet::Detach(c->as);
        free_owned(c->net_phys, KldNet::KLD_NET_PAGES);
        c->net_phys = 0;
    }
    // 4) broker refs on dma + doorbell frames. the mapping refs die with the
    //    address space (or were dropped by explicit DMA_FREE). (satoru)
    for (int i = 0; i < MAX_DMA; i++) {
        if (!c->dma[i].used) continue;
        free_owned(c->dma[i].phys, c->dma[i].pages);
        c->dma[i].used = false;
    }
    if (c->irq_page_phys) { free_owned(c->irq_page_phys, 1); c->irq_page_phys = 0; }
    // 5) grants. (satoru)
    for (int i = 0; i < MAX_GRANTS; i++) {
        if (g_grants[i].used && g_grants[i].as == c->as) {
            log_bdf("[kldb] released ", g_grants[i].bdf);
            SerialLogger::Log(" (bus master off, msi off, frames returned)\r\n");
            g_grants[i].used = false;
        }
    }
    c->used = false;
}

// ── ACPI container support ──────────────────────────────────────────────────
struct MapEnt { uint64_t base, len; uint32_t type; };
MapEnt g_map[64];
int g_map_n = 0;
uint64_t g_acpi_as = 0;
uint32_t g_acpi_refused = 0;

bool is_acpi(uint64_t as) { return g_acpi_as && as == g_acpi_as; }

// type of the firmware map entry covering phys, or 0 if none. (satoru)
uint32_t map_type(uint64_t phys) {
    for (int i = 0; i < g_map_n; i++)
        if (phys >= g_map[i].base && phys < g_map[i].base + g_map[i].len) return g_map[i].type;
    return 0;
}

int64_t op_acpi_rsdp_read(uint32_t off) {
    uint64_t r = BootAcpi::Rsdp();
    if (!r || off > 32 || (off & 7)) return KLD_EINVAL;
    const uint8_t* p = (const uint8_t*)(uintptr_t)r;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[off + i] << (8 * i);
    return (int64_t)v;
}

int64_t op_acpi_map_phys(uint64_t as, uint64_t phys, uint64_t bytes, uint64_t va) {
    if (!g_map_n) return KLD_EPERM;   // no firmware map: refuse everything (satoru)
    uint64_t base = phys & ~0xFFFull;
    uint64_t pages = ((phys + bytes + 0xFFF) & ~0xFFFull) - base;
    pages /= PAGE_SIZE;
    if (!pages || pages > 4096) return KLD_EINVAL;
    bool all_acpi = true;
    for (uint64_t i = 0; i < pages; i++) {
        uint32_t t = map_type(base + i * PAGE_SIZE);
        if (t == 1) {   // usable ram: the kernel's memory, never mapped out (satoru)
            if (g_acpi_refused++ < 4) {
                SerialLogger::Log("[kldb] acpi MAP_PHYS refused (usable ram) 0x");
                SerialLogger::LogHex64(base + i * PAGE_SIZE);
                SerialLogger::Log("\r\n");
            }
            return KLD_EPERM;
        }
        if (t != 3 && t != 4) all_acpi = false;
    }
    if (!va_range_free(as, va, pages)) return KLD_EBUSY;
    for (uint64_t i = 0; i < pages; i++) {
        PMM::RetainFrame(base + i * PAGE_SIZE);
        PMM::RetainFrame(base + i * PAGE_SIZE);
    }
    // acpi reclaim/nvs is ordinary memory (write-back); anything else is
    // treated as mmio (uncached). (satoru)
    uint64_t fl = PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_NX;
    if (!all_acpi) fl |= PTE_PCD | PTE_PWT;
    if (!map_pages(as, va, base, pages, fl)) return KLD_ENOMEM;
    return KLD_OK;
}

// ports the kernel drives itself. everything else (pm1/gpe blocks, smi_cmd,
// the embedded controller, smbus) is the acpi container's to use. (satoru)
bool port_denied(uint32_t port, uint32_t width) {
    uint32_t end = port + width / 8;
    struct { uint32_t lo, hi; } deny[] = {
        { 0x0020, 0x0022 }, { 0x00A0, 0x00A2 },   // 8259 pics (satoru)
        { 0x0040, 0x0044 },                       // pit (satoru)
        { 0x0060, 0x0061 }, { 0x0064, 0x0065 },   // 8042 (satoru)
        { 0x0070, 0x0072 },                       // cmos/rtc index (satoru)
        { 0x03F8, 0x0400 }, { 0x02F8, 0x0300 },   // com1/com2 (satoru)
        { 0x0CF8, 0x0D00 },                       // pci config mechanism (use the op) (satoru)
        { 0x0000, 0x0020 }, { 0x00C0, 0x00E0 },   // isa dma (satoru)
    };
    for (auto& d : deny)
        if (port < d.hi && end > d.lo) return true;
    return false;
}

int64_t op_acpi_port(uint32_t port, uint32_t width, bool out, uint32_t val) {
    if (width != 8 && width != 16 && width != 32) return KLD_EINVAL;
    if (port > 0xFFFF || port_denied(port, width)) return KLD_EPERM;
    if (out) {
        if (width == 8) HAL::OutByte((uint16_t)port, (uint8_t)val);
        else if (width == 16) HAL::OutWord((uint16_t)port, (uint16_t)val);
        else HAL::OutLong((uint16_t)port, val);
        return KLD_OK;
    }
    if (width == 8) return HAL::InByte((uint16_t)port);
    if (width == 16) return HAL::InWord((uint16_t)port);
    return (int64_t)HAL::InLong((uint16_t)port);
}

int64_t op_acpi_pci(uint32_t bdf, uint32_t off, uint32_t width, bool write, uint32_t val) {
    if (!bdf_valid(bdf) || off > 0xFF) return KLD_EINVAL;
    if (width != 1 && width != 2 && width != 4) return KLD_EINVAL;
    if (!write) return (int64_t)cfg_read(bdf, off, width);
    if (cfg_write_refused(bdf, off, width)) return KLD_EPERM;
    cfg_write(bdf, off, width, val);
    return KLD_OK;
}

}  // namespace

void Init() {
    for (int i = 0; i < MAX_GRANTS; i++) g_grants[i].used = false;
    for (int i = 0; i < MAX_CONTAINERS; i++) g_cont[i].used = false;
    for (int i = 0; i < (int)MSI_VECTOR_COUNT; i++) g_msi[i].used = false;
    SerialLogger::Log("[kldb] KLD broker initialized (pci proxy, bar map, dma, msi, net bridge)\r\n");
}

void SetIrqWake(bool on) { g_irq_wake = on; }

void SnapshotMemoryMap(const void* mbi_v) {
    const multiboot_info_t* mbi = (const multiboot_info_t*)mbi_v;
    g_map_n = 0;
    if (!mbi || !(mbi->flags & (1u << 6))) return;
    const uint8_t* p = (const uint8_t*)(uintptr_t)mbi->mmap_addr;
    const uint8_t* end = p + mbi->mmap_length;
    while (p + 24 <= end && g_map_n < 64) {
        uint32_t sz = *(const uint32_t*)p;
        g_map[g_map_n].base = *(const uint64_t*)(p + 4);
        g_map[g_map_n].len  = *(const uint64_t*)(p + 12);
        g_map[g_map_n].type = *(const uint32_t*)(p + 20);
        g_map_n++;
        p += sz + 4;
    }
}

void GrantAcpi(uint64_t address_space) {
    g_acpi_as = address_space;
    SerialLogger::Log(address_space ? "[kldb] acpi privilege granted to the kld-acpi container\r\n"
                                    : "[kldb] acpi privilege revoked\r\n");
}

void OnVector(uint32_t vector) {
    uint32_t h = vector - MSI_VECTOR_BASE;
    if (h >= MSI_VECTOR_COUNT) return;
    g_vec_hits[h]++;
    MsiRec* m = &g_msi[h];
    if (!__atomic_load_n(&m->used, __ATOMIC_ACQUIRE)) return;
    m->fired++;
    __atomic_fetch_add(m->counter, 1u, __ATOMIC_RELEASE);
    if (g_irq_wake) LinuxSyscallFutexWakePhys(m->counter_phys, 0x7FFFFFFF);
}

int64_t Call(Process* cur, uint32_t op, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    if (!cur || !cur->is_user() || !cur->address_space) return KLD_EPERM;
    // an untrusted container can reach real hardware through this broker, so
    // only a privileged process (uid 0 / approved) may call it. (satoru)
    if (!AdminGate::Authorize("hardware/driver access", "kld broker call")) return KLD_EPERM;
    uint64_t as = cur->address_space;
    int pid = (int)cur->pid;
    switch (op) {
        case KLD_OP_PCI_FIND:
            return op_pci_find(as, pid, (uint32_t)a0, (uint32_t)a1, (uint32_t)a2);
        case KLD_OP_PCI_CFG_READ:
            return op_cfg_read(as, (uint32_t)a0, (uint32_t)a1, (uint32_t)a2);
        case KLD_OP_PCI_CFG_WRITE:
            return op_cfg_write(as, (uint32_t)a0, (uint32_t)a1, (uint32_t)a2, (uint32_t)a3);
        case KLD_OP_PCI_BAR:
            return op_pci_bar(as, (uint32_t)a0, (uint32_t)a1);
        case KLD_OP_PCI_BAR_SIZE:
            return op_pci_bar_size(as, (uint32_t)a0, (uint32_t)a1);
        case KLD_OP_PCI_ENABLE:
            return op_pci_enable(as, (uint32_t)a0);
        case KLD_OP_PCI_DISABLE:
            return op_pci_disable(as, (uint32_t)a0);
        case KLD_OP_MAP_BAR:
            return op_map_bar(as, (uint32_t)a0, (uint32_t)a1, a2, a3);
        case KLD_OP_DMA_ALLOC:
            return op_dma_alloc(as, pid, a0, a1, (uint32_t)a2);
        case KLD_OP_DMA_FREE:
            return op_dma_free(as, a0);
        case KLD_OP_IRQ_PAGE:
            return op_irq_page(as, pid, a0);
        case KLD_OP_MSI_ALLOC:
            return op_msi_alloc(as, (uint32_t)a0);
        case KLD_OP_MSI_FREE:
            return op_msi_free(as, (uint32_t)a0);
        case KLD_OP_MSIX_ALLOC:
            return op_msix_alloc(as, (uint32_t)a0, (uint32_t)a1);
        case KLD_OP_MSIX_FREE:
            return op_msix_free(as, (uint32_t)a0);
        case KLD_OP_MSIX_COUNT:
            return op_msix_count(as, (uint32_t)a0);
        case KLD_OP_NET_ATTACH:
            return op_net_attach(as, pid, a0, a1, a2);
        case KLD_OP_NET_CARRIER:
            return KldNet::SetCarrier(as, a0 != 0);
        case KLD_OP_NET_RX_KICK:
            return KldNet::RxKick(as);
        case KLD_OP_NET_TICK:
            return KldNet::Tick(as);
        case KLD_OP_ACPI_RSDP_READ:
            if (!is_acpi(as)) return KLD_EPERM;
            return op_acpi_rsdp_read((uint32_t)a0);
        case KLD_OP_ACPI_MAP_PHYS:
            if (!is_acpi(as)) return KLD_EPERM;
            return op_acpi_map_phys(as, a0, a1, a2);
        case KLD_OP_ACPI_PORT_IN:
            if (!is_acpi(as)) return KLD_EPERM;
            return op_acpi_port((uint32_t)a0, (uint32_t)a1, false, 0);
        case KLD_OP_ACPI_PORT_OUT:
            if (!is_acpi(as)) return KLD_EPERM;
            return op_acpi_port((uint32_t)a0, (uint32_t)a1, true, (uint32_t)a2);
        case KLD_OP_ACPI_PCI_READ:
            if (!is_acpi(as)) return KLD_EPERM;
            return op_acpi_pci((uint32_t)a0, (uint32_t)a1, (uint32_t)a2, false, 0);
        case KLD_OP_ACPI_PCI_WRITE:
            if (!is_acpi(as)) return KLD_EPERM;
            return op_acpi_pci((uint32_t)a0, (uint32_t)a1, (uint32_t)a2, true, (uint32_t)a3);
        case KLD_OP_STATUS: {
            // a0=1+h: raw isr delivery count of vector h, for diagnostics. (satoru)
            if (a0 >= 1 && a0 <= MSI_VECTOR_COUNT) return (int64_t)g_vec_hits[a0 - 1];
            int n = 0;
            for (int i = 0; i < MAX_GRANTS; i++)
                if (g_grants[i].used && g_grants[i].as == as) n++;
            return (int64_t)n;
        }
        case KLD_OP_RELEASE: {
            Container* c = find_container(as, false, pid);
            if (c) release_container(c);
            return KLD_OK;
        }
        default:
            return KLD_EINVAL;
    }
}

void ReleaseAddressSpace(uint64_t address_space) {
    Container* c = find_container(address_space, false, -1);
    if (c) release_container(c);
    // grants without a container record (find only). (satoru)
    for (int i = 0; i < MAX_GRANTS; i++)
        if (g_grants[i].used && g_grants[i].as == address_space) g_grants[i].used = false;
}

int Status(char* out, int mx) {
    int p = 0;
    auto cat = [&](const char* s) { while (*s && p < mx - 1) out[p++] = *s++; out[p] = 0; };
    auto cati = [&](int v) {
        char r[12]; int ri = 0;
        if (v == 0) r[ri++] = '0';
        else { char t[12]; int ti = 0; int x = v; while (x) { t[ti++] = (char)('0' + x % 10); x /= 10; } while (ti) r[ri++] = t[--ti]; }
        r[ri] = 0; cat(r);
    };
    cat("KLD broker (ring-3 linux driver containers):\n");
    cat("  pci_find calls: "); cati((int)g_find_calls); cat("\n");
    int n = 0;
    for (int i = 0; i < MAX_GRANTS; i++) {
        if (!g_grants[i].used) continue;
        n++;
        cat("  grant pid="); cati(g_grants[i].pid);
        cat(" bdf=");        cati((int)bdf_bus(g_grants[i].bdf));
        cat(":");            cati((int)bdf_dev(g_grants[i].bdf));
        cat(".");            cati((int)bdf_fn(g_grants[i].bdf));
        cat(" cfgr=");       cati((int)g_grants[i].cfg_reads);
        cat(" cfgw=");       cati((int)g_grants[i].cfg_writes);
        cat(" refused=");    cati((int)g_grants[i].cfg_refused);
        cat("\n");
    }
    for (int h = 0; h < (int)MSI_VECTOR_COUNT; h++) {
        if (!g_msi[h].used) continue;
        cat("  msi handle "); cati(h); cat(" fired="); cati((int)g_msi[h].fired); cat("\n");
    }
    if (!n) cat("  (no devices granted)\n");
    return p;
}

}  // namespace KldBroker

// end (satoru)
