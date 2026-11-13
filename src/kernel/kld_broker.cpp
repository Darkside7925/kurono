//  KLD broker implementation - see kld_broker.h for the design and the honest
//  limits of the isolation it provides.
//
//  end (satoru)

#include "kld_broker.h"
#include "kld_auto.h"
#include "kld_ioapic.h"
#include "kld_input.h"
#include "kld_display.h"
#include "kld_iommu.h"
#include "kld_model.h"
#include "kld_dri.h"   // drm userspace interface, ops 0xA0..0xBF (satoru)
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "../drivers/serial.h"
#include "../proc/scheduler.h"
#include "../proc/smp.h"
#include "../security/admin.h"   // kld broker requires administrator approval (satoru)
#include "../proc/spinlock.h"
#include "multiboot.h"
#include "kld_supervisor.h"   // heartbeat table (satoru)
#include "../linux/linux_syscall.h"   // KlsDropAll around the rig free delay (satoru)
#include "../hal/hal.h"
#include "../drivers/cpu_detect.h"
#include "../drivers/timer.h"
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

static inline uint64_t n1b_rdtsc() {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
// doorbell page layout shared with lx_irq.c: u32 counter per handle at 4*h,
// u64 N1b t0 (kernel MSI entry TSC) per handle at 2048 + 8*h. (satoru)
static constexpr uint64_t KLD_IRQ_T0_OFF = 2048;
static uint64_t g_n1b_irqs, g_n1b_wakes;
static bool g_kldstats;

// N1c: was the container's irq thread Blocked when the isr forwarded this
// vector, did the wake find (unblock) it, and did it then fail to run? all
// reads of scheduler state: nothing here changes scheduling. (satoru)
static uint64_t g_n1c_calls, g_n1c_found, g_n1c_none, g_n1c_notrun;
static uint64_t g_n1c_last_print_ms;
struct N1cPrev { uint32_t pid; uint64_t t0_ms; uint32_t save_seq; uint8_t state; };
static N1cPrev g_n1c_prev;
static bool g_n1c_prev_ok;
// the container threads all carry the executable's name, so the irq thread is
// found by its transition: snapshot every Blocked task of the container right
// before the wake and look for the one the wake unblocked. (satoru)
struct N1cSnap { uint32_t pid; uint32_t seq; };
static int n1c_blocked_snapshot(uint64_t as, N1cSnap* out, int max) {
    uint32_t pid = Scheduler::FindLivePidInAddressSpace(as);
    if (!pid) return 0;
    Process* g[32];
    int n = Scheduler::CollectAddressSpaceGroup(pid, g, 32);
    int k = 0;
    for (int i = 0; i < n && k < max; i++) {
        if (!g[i] || g[i]->state != Process_Blocked) continue;
        out[k].pid = g[i]->pid;
        out[k].seq = g[i]->save_seq;
        k++;
    }
    return k;
}
static const char* n1c_cpu_task(uint32_t c, bool* idle) {
    PerCpu* pc = SMP::ByIndex(c);
    if (idle) *idle = !pc || SMP::DebugIdle(c) || !pc->current;
    if (!pc || !pc->current) return "idle";
    return pc->current->name;
}
// kurono.kld.nomsi=1: make the MSI/MSI-X ops fail so a driver falls back to
// its PCI INTx path. the C5.3 line-irq proof uses it. (satoru)
bool g_nomsi = false;

// ── grant table ─────────────────────────────────────────────────────────────
// one entry per (container address space, device) pair. small and fixed: a
// handful of driver containers is the realistic ceiling, and a fixed table
// means no allocation on a path an untrusted process can drive. (satoru)
constexpr int MAX_GRANTS = 16;

struct Grant {
    bool     used;
    // the device is in this container's own vt-d domain (Attach succeeded),
    // or it is the model device (no dma at all): the ggtt half of bar0 may be
    // writable under kurono.kld.bar=window (review g2). (satoru)
    bool     dma_isolated;
    uint64_t as;         // container address space = the grant key (satoru)
    int      pid;        // the thread that asked, for the log only (satoru)
    uint32_t bdf;
    uint32_t cfg_reads;
    uint32_t cfg_writes;
    uint32_t cfg_refused;
    // (satoru) this lets a granted BAR page that CONTAINS the MSI-X table also
    // hold device registers (BE200 BAR0: table at 0x2000, CSR_MSIX regs at
    // 0x2800 in the same 4 KiB page). the container could then rewrite MSI-X
    // entries; that is acceptable only because a container is already trusted
    // with DMA until the broker uses an IOMMU. it is set from the matched
    // family's manifest property (needs_msix_table_page) after the device id
    // matched that family's own table, never requested by the container, and
    // pages holding only the PBA stay refused.
    bool     allow_msix_table_page;
    // the family manifest said legacy_vga=1; the port ops additionally require
    // this device to be the machine's only io-decoding display adapter (or the
    // explicit boot flag). (satoru)
    bool     legacy_vga;
    // the grant's address space was destroyed: the device stays held until the
    // release frees the grant, but no lookup by that address space matches it
    // any more (a later process can get the same PML4 frame). (satoru)
    bool     dead;
};

// a grant this address space may use now. (satoru)
inline bool grant_live(const Grant& g, uint64_t as) {
    return g.used && g.as == as && !g.dead;
}

Grant g_grants[MAX_GRANTS];
uint32_t g_find_calls = 0;
bool g_legacy_vga_forced = false;

// ── per-container resources ─────────────────────────────────────────────────
constexpr int MAX_CONTAINERS = 4;
// 256 MiB / 256 KiB shim arena chunks = 1024 records, so the full iwlwifi
// budget can be handed out without hitting the table first. (satoru)
constexpr int MAX_DMA        = 1024;
constexpr int MAX_MAPS       = 128;

// freeing: DMA_FREE has passed its alias check; no new alias may take its
// frames (set and tested under g_alias_lock). (satoru)
struct DmaRec { bool used; uint64_t va; uint64_t phys; uint32_t pages; bool freeing; };
struct MapRec { bool used; bool plat; uint64_t va; uint64_t phys; uint32_t pages; };

// a vmap() alias of DMA frames the container already owns: the pages of a
// ttm system buffer are scattered over the shim's arena, so vmap maps them
// again, run by run, at one fresh VA (KLD_OP_DMA_ALIAS). policy:
//  - only frames of this container's own DMA_ALLOC records; never a BAR,
//    a display/net/irq frame or another container's frame;
//  - each aliased frame holds two refs, like every broker map: the mapping
//    ref (dropped by the address-space teardown or by UNALIAS's unmap) and
//    the alias ref (dropped by UNALIAS or by release_container, which never
//    touches the container's ptes);
//  - the whole reserved range is a broker range (RangeIsBrokerMapping) from
//    before the first pte until UNALIAS, so the container cannot munmap,
//    madvise, mprotect or remap it;
//  - plain write-back pages (RAM, no pat/pcd/pwt bits); write-combining is
//    only ever for a BAR mapping;
//  - limits: MAX_ALIAS records in the machine, MAX_ALIAS_PER_CONT per
//    container, MAX_ALIAS_PAGES per record. (satoru)
constexpr int MAX_ALIAS = 8;
constexpr int MAX_ALIAS_PER_CONT = 4;
// 16 MiB per record: a 1920x1080 xrgb buffer is 2025 pages; a 3840x2160 one
// (8100 pages) does not fit and its vmap fails cleanly. (satoru)
constexpr uint32_t MAX_ALIAS_PAGES = 4096;
struct AliasRec {
    bool     used;
    bool     busy;       // a run is being mapped or the record torn down (satoru)
    bool     orphan;     // released while busy: the busy op finishes the release (satoru)
    uint64_t as;
    uint32_t gen;        // the owning container record (as keys can be reused) (satoru)
    uint64_t va;
    uint32_t reserved;   // pages covered as a broker range (satoru)
    uint32_t pages;      // pages mapped, phys[0..pages) valid (satoru)
    uint64_t pend_phys;  // the run being mapped while busy (satoru)
    uint32_t pend_pages;
    uint64_t phys[MAX_ALIAS_PAGES];
};
AliasRec g_alias[MAX_ALIAS];
Spinlock g_alias_lock;

struct Container {
    bool     used;
    uint64_t as;
    int      pid;
    char     family[16];      // kld family name from the runner path (satoru)
    uint64_t irq_page_phys;   // doorbell page (broker-owned frame) (satoru)
    uint64_t irq_page_va;
    uint64_t net_phys;        // shared packet rings (satoru)
    uint64_t net_va;
    bool     legacy_vga_warned;
    bool     as_gone;         // the address space was destroyed; never match it (satoru)
    bool     pref_bdf_valid;  // the auto matcher selected one adapter: only it (satoru)
    uint32_t pref_bdf;
    uint32_t disp_id;         // display instance id (0 = none; D2.8) (satoru)
    DmaRec   dma[MAX_DMA];
    MapRec   maps[MAX_MAPS];
    uint32_t dma_pages;
    uint32_t dma_limit_pages;   // default 16384 (64 MiB) (satoru)
    uint32_t gen;               // instance id, unique per record creation (satoru)
    uint32_t plat_pages;        // PLAT_MAP budget, 4 MiB total (satoru)
    uint32_t plat_maps;         // PLAT_MAP count, 32 max (satoru)
    uint32_t acpi_map_pages;    // broker MAP_PHYS budget, 16 MiB (satoru)
    // acpi provider accounting: the kernel counts what actually reached the
    // hardware vs what was refused and shadowed. (satoru)
    bool     acpi_touched;
    uint32_t acpi_real_port_w;
    uint32_t acpi_real_pci_w;
    uint32_t acpi_shadow_port_w;
    uint32_t acpi_shadow_pci_w;
    struct { bool used; bool pci; uint32_t addr, width, value; } acpi_shadow[16];
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
    volatile uint64_t* t0slot;          // N1b kernel-entry TSC for this handle (satoru)
    volatile uint32_t fired;
    // ── line interrupts (IRQ_LINE_ATTACH) ──────────────────────────────────
    bool     line;                      // this slot is an I/O APIC line (satoru)
    uint32_t gsi;                       // resolved I/O APIC GSI (satoru)
    uint32_t line_flags;                // KldIoApic::LINE_* as attached (satoru)
    uint64_t storm_ms;                  // start of the current unclaimed window (satoru)
    uint32_t storm_count;               // unclaimed unmask calls in that window (satoru)
    uint64_t rate_ms;                   // 100 ms delivery-rate window (isr side) (satoru)
    uint32_t rate_count;                // deliveries in that window (satoru)
    bool     rate_masked;               // over budget: masked until the next window (satoru)
    bool     eoi_suppress;              // SVR bit 12 at attach: write EOIR (satoru)
    bool     storm_masked;              // guard tripped: masked until detach (satoru)
    uint32_t gen;                       // Container::gen of the owner at attach: a line record belongs to ONE container instance, not to the address space key (satoru)
};

MsiRec g_msi[MSI_VECTOR_COUNT];
volatile uint32_t g_vec_hits[MSI_VECTOR_COUNT];   // every delivery, used or not (satoru)
// the gsi an isr must mask even when the slot was just detached (the handle can
// be reused, the vector cannot be unmasked by a racing ownerless detach). (satoru)
uint32_t g_line_gsi[MSI_VECTOR_COUNT];
// deliveries currently past the used-check: release waits for zero before it
// frees the doorbell page. incremented in OnVector, decremented on every exit.
volatile uint32_t g_vec_in_flight[MSI_VECTOR_COUNT];

void line_reset(int h);
uint32_t family_flags_of(Container* c);
uint32_t map_type_any(uint64_t phys, uint64_t len);

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
        if (grant_live(g_grants[i], as) && g_grants[i].bdf == bdf)
            return &g_grants[i];
    return nullptr;
}

// a device may be granted to ONE container at a time: two drivers fighting
// over a nic is a bug class we refuse rather than debug. (satoru)
bool granted_elsewhere(uint64_t as, uint32_t bdf) {
    for (int i = 0; i < MAX_GRANTS; i++)
        if (g_grants[i].used && g_grants[i].bdf == bdf &&
            (g_grants[i].as != as || g_grants[i].dead))   // a dead space still holds it (satoru)
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

void remove_grant(uint64_t as, uint32_t bdf) {
    Grant* g = find_grant(as, bdf);
    if (g) g->used = false;
}

Container* find_container(uint64_t as, bool create, int pid) {
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (g_cont[i].used && g_cont[i].as == as && !g_cont[i].as_gone) {
            // a later find with the real pid updates the diagnostic copy. (satoru)
            if (pid >= 0) g_cont[i].pid = pid;
            return &g_cont[i];
        }
    if (!create) return nullptr;
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (!g_cont[i].used) {
            g_cont[i] = {};
            g_cont[i].used = true;
            g_cont[i].as = as;
            g_cont[i].pid = pid;
            g_cont[i].dma_limit_pages = 16384;   // 64 MiB default (satoru)
            static uint32_t s_gen = 0;
            g_cont[i].gen = ++s_gen;
            return &g_cont[i];
        }
    }
    return nullptr;
}

// only the release path may still see an address-space-dead record: it is the
// caller that frees that container's grants and frames. every other non-create
// lookup skips it, so a later process on the same PML4 frame gets EPERM rather
// than inheriting a dead container. (satoru)
Container* find_container_release(uint64_t as) {
    Container* fallback = nullptr;
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (!g_cont[i].used || g_cont[i].as != as) continue;
        if (g_cont[i].as_gone) return &g_cont[i];
        if (!fallback) fallback = &g_cont[i];
    }
    return fallback;
}

// the grants a container record owns: same address space; when a new live
// container already reuses that key (only possible with the direct map),
// only the dead ones. never decided by the dead flag otherwise: the destroy
// hook can mark them while a release runs on another cpu (the d3 kill loop
// leaked a grant exactly that way). (satoru)
inline bool grant_of(const Grant& g, uint64_t as, bool key_reused) {
    return g.used && g.as == as && (!key_reused || g.dead);
}

// the runner path is /usr/bin/kld-<family>; that is the broker's only record
// of which family's table a container may use for the msi-x exception. (satoru)
void family_from_path(const char* path, char* out, int mx)
{
    out[0] = 0;
    if (!path) return;
    const char* base = path;
    for (const char* p = path; *p; p++)
        if (*p == '/') base = p + 1;
    if (base[0] != 'k' || base[1] != 'l' || base[2] != 'd' || base[3] != '-')
        return;
    base += 4;
    int i = 0;
    while (base[i] && i < mx - 1) { out[i] = base[i]; i++; }
    out[i] = 0;
}

// the no-table and no-family messages are diagnostics, not per-call spam.
// (satoru)
bool g_logged_no_table;
uint32_t g_logged_nofam[8];
int g_logged_nofam_n;

void log_nofam_once(uint16_t vendor, uint16_t device)
{
    uint32_t key = ((uint32_t)vendor << 16) | device;
    for (int i = 0; i < g_logged_nofam_n; i++)
        if (g_logged_nofam[i] == key)
            return;
    if (g_logged_nofam_n < 8)
        g_logged_nofam[g_logged_nofam_n++] = key;
    SerialLogger::Log("[kldb] ");
    SerialLogger::LogHex(vendor);
    SerialLogger::Log(":");
    SerialLogger::LogHex(device);
    SerialLogger::Log(" matches no family table (msi-x table page stays refused)\r\n");
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
    if (KldModel::IsModel(bdf)) return KldModel::CfgRead(off & 0xFCu, 4);   // (satoru)
    SpinLockGuard g(g_cfg_lock);
    return PCI::Read32(bdf_bus(bdf), bdf_dev(bdf), bdf_fn(bdf), (uint8_t)(off & 0xFC));
}

void cfg_w32(uint32_t bdf, uint32_t off, uint32_t v) {
    if (KldModel::IsModel(bdf)) { KldModel::CfgWrite(off & 0xFCu, 4, v); return; }   // (satoru)
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
    if (KldModel::IsModel(bdf)) { KldModel::CfgWrite(off, width, val); return; }   // (satoru)
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
bool is_display_class(uint32_t bdf);

// display adapters (review f2): 0xE0-0xE8 are the graphics swsci/asle
// registers (a write raises an smi into firmware) and 0xFC is asls, the
// source of the opregion copy. no container writes them; the i915 path writes
// only the command register (through PCI_ENABLE/DISABLE) and msi (broker).
// one rule for real devices and the model device. (satoru)
bool cfg_write_refused(uint32_t bdf, uint32_t off, uint32_t width) {
    if (is_display_class(bdf) &&
        (overlaps(off, width, 0xE0, 0xE9) || overlaps(off, width, 0xFC, 0x100)))
        return true;
    if (overlaps(off, width, 0x10, 0x28)) return true;
    if (overlaps(off, width, 0x30, 0x34)) return true;
    uint8_t msi = find_cap(bdf, 0x05);
    if (msi && overlaps(off, width, msi, msi + 0x18u)) return true;
    uint8_t msix = find_cap(bdf, 0x11);
    if (msix && overlaps(off, width, msix, msix + 0x0Cu)) return true;
    return false;
}

// ── bar decoding ────────────────────────────────────────────────────────────
// true when bar idx is the upper dword of the 64-bit memory bar idx-1: it is
// not a bar of its own and is never mapped or sized. (satoru)
bool bar_is_upper_half(uint32_t bdf, uint32_t idx) {
    if (idx == 0 || idx > 5) return false;
    uint32_t prev = cfg_r32(bdf, 0x10 + (idx - 1) * 4);
    return !(prev & 1) && ((prev >> 1) & 3) == 2;
}

// returns the physical base of a memory bar (0 for io / unused / upper half).
uint64_t bar_base(uint32_t bdf, uint32_t idx, bool* is64) {
    *is64 = false;
    if (bar_is_upper_half(bdf, idx)) return 0;   // (satoru)
    uint32_t lo = cfg_r32(bdf, 0x10 + idx * 4);
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
    if (KldModel::IsModel(bdf)) return KldModel::BarSize(idx);   // (satoru)
    // never write all-ones into the upper dword of a live 64-bit bar. (satoru)
    if (bar_is_upper_half(bdf, idx)) return 0;
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

// kurono.kld.tlbskip=1 (rig): the next broker shootdown is skipped once, so
// the frames of that unmap must wait in the proven-free queue. (satoru)
bool g_tlbskip_armed;
bool g_tlbtest_entry;     // the next queued entry is the test's (satoru)

// shootdown after clearing ptes of a container. best effort: it can give up
// on a cpu that does not ack; frees therefore go through FreeAfterTlbProof.
// (satoru)
void broker_tlb_flush() {
    if (g_tlbskip_armed) {
        g_tlbskip_armed = false;
        g_tlbtest_entry = true;
        SerialLogger::Log("[kldb] tlbq test: broadcast skipped for this unmap\r\n");
        return;
    }
    if (SMP::OnlineCount() > 1) SMP::BroadcastTlbFlush();
}

void unmap_pages(uint64_t as, uint64_t va, uint64_t pages) {
    for (uint64_t i = 0; i < pages; i++)
        KernelVMM::UnmapPageInAddressSpace(as, va + i * PAGE_SIZE, false);
    // other cores may run threads of this container. (satoru)
    broker_tlb_flush();
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

// ── tlb-proven free ─────────────────────────────────────────────────────────
// a frame just unmapped from a LIVE container address space may still sit in
// another cpu's tlb: BroadcastTlbFlush gives up after 20 ms on a cpu that does
// not ack. so the refs of such frames are dropped only once a shootdown that
// STARTED after the ptes were cleared has been acked by every cpu
// (SMP::TlbFlushFullSeq() > the stamp taken at enqueue), the proof
// user_frame_quarantine in linux_syscall.cpp uses. bounded queue; when full
// the entry is leaked with one [kld-warn], never freed unproven. frames whose
// address space is already destroyed need no proof (no tlb can hold that
// root's translations once the vmm has torn it down). (satoru)
struct TlbQ { uint64_t phys; uint32_t pages; uint32_t refs; uint64_t seq; bool test; };
constexpr int TLBQ_SLOTS = 512;
TlbQ g_tlbq[TLBQ_SLOTS];
int g_tlbq_head, g_tlbq_tail;
Spinlock g_tlbq_lock;
uint32_t g_tlbq_leaked;
bool g_tlbq_warned;

void tlbq_release_locked(const TlbQ& e, uint64_t full) {
    for (uint32_t i = 0; i < e.pages; i++)
        for (uint32_t k = 0; k < e.refs; k++)
            PMM::FreeFrame(e.phys + (uint64_t)i * PAGE_SIZE);
    if (e.test) {
        SerialLogger::Log("[kldb] tlbq test: released ");
        SerialLogger::LogDec((int)e.pages);
        SerialLogger::Log(" pages after a proven flush, full=");
        SerialLogger::LogDec((int)full);
        SerialLogger::Log(" > stamp=");
        SerialLogger::LogDec((int)e.seq);
        SerialLogger::Log("\r\n");
    }
}

// drop every entry whose stamp a full flush has passed; buy_proof: run one
// broadcast first (only from a context where the cpu may wait for acks: a
// syscall or a kernel process with interrupts on). (satoru)
void tlbq_drain(bool buy_proof) {
    if (buy_proof && SMP::OnlineCount() > 1) {
        bool any;
        {
            SpinLockGuard g(g_tlbq_lock);
            any = g_tlbq_head != g_tlbq_tail;
        }
        if (any) SMP::BroadcastTlbFlush();
    }
    SpinLockGuard g(g_tlbq_lock);
    uint64_t full = SMP::TlbFlushFullSeq();
    while (g_tlbq_tail != g_tlbq_head && full > g_tlbq[g_tlbq_tail].seq) {
        tlbq_release_locked(g_tlbq[g_tlbq_tail], full);
        g_tlbq_tail = (g_tlbq_tail + 1) % TLBQ_SLOTS;
    }
}

// ptes of [phys, +pages) were just cleared in a live address space (and a
// shootdown attempted): drop `refs` refs of each frame once that is proven.
void free_after_tlb_proof(uint64_t phys, uint64_t pages, uint32_t refs) {
    if (!pages || !refs) return;
    if (SMP::OnlineCount() <= 1 && !g_tlbtest_entry) {
        for (uint64_t i = 0; i < pages; i++)
            for (uint32_t k = 0; k < refs; k++) PMM::FreeFrame(phys + i * PAGE_SIZE);
        return;
    }
    SpinLockGuard g(g_tlbq_lock);
    uint64_t full = SMP::TlbFlushFullSeq();
    while (g_tlbq_tail != g_tlbq_head && full > g_tlbq[g_tlbq_tail].seq) {
        tlbq_release_locked(g_tlbq[g_tlbq_tail], full);
        g_tlbq_tail = (g_tlbq_tail + 1) % TLBQ_SLOTS;
    }
    int next = (g_tlbq_head + 1) % TLBQ_SLOTS;
    if (next == g_tlbq_tail) {
        g_tlbq_leaked += (uint32_t)pages;
        if (!g_tlbq_warned) {
            g_tlbq_warned = true;
            SerialLogger::Log("[kld-warn] broker tlb-proven free queue full: frames leaked, never freed unproven\r\n");
        }
        return;
    }
    TlbQ& e = g_tlbq[g_tlbq_head];
    e.phys = phys; e.pages = (uint32_t)pages; e.refs = refs;
    e.seq = SMP::TlbFlushStartSeq();
    e.test = g_tlbtest_entry;
    if (e.test) {
        g_tlbtest_entry = false;
        SerialLogger::Log("[kldb] tlbq test: ");
        SerialLogger::LogDec((int)pages);
        SerialLogger::Log(" pages queued, stamp=");
        SerialLogger::LogDec((int)e.seq);
        SerialLogger::Log(" full=");
        SerialLogger::LogDec((int)full);
        SerialLogger::Log("\r\n");
    }
    g_tlbq_head = next;
}

// ── ops ─────────────────────────────────────────────────────────────────────

// ── the device cache ────────────────────────────────────────────────────────
// A bus scan MUST be cached. The first version scanned 256x32x8 on every
// PCI_FIND, and a driver with a 115-entry id_table (e1000e) therefore drove
// ~7.5 MILLION config reads, each two port-I/O cycles - it never finished.
// Scan once into this table, then FIND is a linear search over a handful of
// entries. (satoru)
constexpr int MAX_DEVS = 256;

struct DevEntry {
    uint32_t bdf;
    uint16_t vendor;
    uint16_t device;
    // the auto-matcher needs the same fields linux's pci_match_one_device
    // compares, so the one scan reads them too. (satoru)
    uint16_t subvendor;
    uint16_t subdevice;
    uint32_t class_code;   // 24 bit, like pci_dev.class (satoru)
    // bar windows, cached ONCE at scan time. an overlap check must never size
    // a bar after boot (bar_size disables decode and writes all-ones). header
    // type 0 has six slots, a bridge (type 1) only two. (satoru)
    uint8_t  hdr_type;
    uint8_t  bar_count;
    uint64_t bar_base[6];
    uint64_t bar_size[6];
};

DevEntry g_devs[MAX_DEVS];
int  g_dev_count = 0;
volatile int g_scan_state;      // 0 cold, 1 scanning, 2 done (satoru)
bool g_scan_truncated;

// all 256 buses. the scan is LAZY (first PCI_FIND), not on the boot path, so
// the laptop's 0x82 card and the LPSS at 0x80 are reachable without adding a
// millisecond to boot. the native igc driver already walks 0..255 the same
// way; there is no cheaper kernel-wide enumeration to reuse. (satoru)
constexpr uint32_t SCAN_BUS_LIMIT = 256;

void scan_bars(DevEntry* e)
{
    e->bar_count = e->hdr_type == 0 ? 6 : (e->hdr_type == 1 ? 2 : 0);
    for (uint32_t idx = 0; idx < 6; idx++) {
        e->bar_base[idx] = 0;
        e->bar_size[idx] = 0;
    }
    for (uint32_t idx = 0; idx < e->bar_count; idx++) {
        bool is64 = false;
        uint64_t base = bar_base(e->bdf, idx, &is64);
        if (!base || base == 0xFFFFFFFFull || (base & 1))   // io bars are not mmio
            continue;
        e->bar_base[idx] = base;
        e->bar_size[idx] = 0;   // filled conservatively after the scan (satoru)
        if (is64 && idx + 1 < e->bar_count) {
            idx++;   // the upper half is not a separate window (satoru)
        }
    }
}

void scan_bus_once() {
    int st = __atomic_load_n(&g_scan_state, __ATOMIC_ACQUIRE);
    if (st == 2) return;
    int expected = 0;
    if (!__atomic_compare_exchange_n(&g_scan_state, &expected, 1, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&g_scan_state, __ATOMIC_ACQUIRE) == 1)
            __asm__ __volatile__("pause");
        return;
    }
    uint64_t scan_t0 = Scheduler::NowMs();
    for (uint32_t bus = 0; bus < SCAN_BUS_LIMIT; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint16_t v = PCI::GetVendor((uint8_t)bus, dev, func);
                if (v == 0xFFFF || v == 0) { if (func == 0) break; continue; }
                if (g_dev_count >= MAX_DEVS) goto done;
                g_devs[g_dev_count].bdf    = PackBdf((uint8_t)bus, dev, func);
                g_devs[g_dev_count].vendor = v;
                g_devs[g_dev_count].device = PCI::GetDevice((uint8_t)bus, dev, func);
                g_devs[g_dev_count].subvendor = (uint16_t)cfg_read(g_devs[g_dev_count].bdf, 0x2C, 2);
                g_devs[g_dev_count].subdevice = (uint16_t)cfg_read(g_devs[g_dev_count].bdf, 0x2E, 2);
                g_devs[g_dev_count].class_code = cfg_read(g_devs[g_dev_count].bdf, 0x08, 4) >> 8;
                g_devs[g_dev_count].hdr_type = PCI::Read8((uint8_t)bus, dev, func, 0x0E) & 0x7F;
                scan_bars(&g_devs[g_dev_count]);
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
    // no decode-off sizing here or later: a bar_size() call writes all-ones
    // into the bar while another cpu may be using the device, and doing it for
    // every unrelated device at scan time is the same risk. a bar is naturally
    // aligned to its size, so its window can never be longer than the lowest
    // set bit of its base; the gap to the next bar base of any device bounds it
    // too. both are UPPER bounds read without a single write: never shorter
    // than the real window (so nothing a bar decodes is mappable), and tight
    // for the small bars that matter (a 4 KiB lpss bar at ...216000 is 4 KiB).
    // the price is that a big bar can protect more than it decodes; the exact
    // size would need the write-ones probe. (satoru)
    for (int i = 0; i < g_dev_count; i++) {
        DevEntry* e = &g_devs[i];
        for (uint32_t idx = 0; idx < e->bar_count; idx++) {
            uint64_t base = e->bar_base[idx];
            if (!base) continue;
            uint64_t next = 0;
            for (int j = 0; j < g_dev_count; j++)
                for (uint32_t k = 0; k < g_devs[j].bar_count; k++) {
                    uint64_t b2 = g_devs[j].bar_base[k];
                    if (b2 > base && (!next || b2 < next)) next = b2;
                }
            uint64_t ub = base & (~base + 1);   // lowest set bit (satoru)
            if (next > base && next - base < ub) ub = next - base;
            if (ub < PAGE_SIZE) ub = PAGE_SIZE;
            e->bar_size[idx] = (ub + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
        }
    }
    if (g_dev_count >= MAX_DEVS) {
        g_scan_truncated = true;
        SerialLogger::Log("[kldb] pci scan TRUNCATED at ");
        SerialLogger::LogDec(g_dev_count);
        SerialLogger::Log(" devices: plat-map bar refusal is incomplete\r\n");
    }
    // kurono.kld.model=: the synthetic test device joins the table like a
    // real one, so the auto matcher and every op see it the same way. its bars
    // are fake bus addresses backed by kernel ram, so it adds no bar window
    // to the platform-map overlap table (bar_count 0). (satoru)
    if (KldModel::Enabled() && g_dev_count < MAX_DEVS) {
        DevEntry& e = g_devs[g_dev_count++];
        e = {};
        e.bdf = KldModel::Bdf();
        KldModel::Identity(&e.vendor, &e.device, &e.subvendor, &e.subdevice,
                           &e.class_code);
    }
    uint64_t scan_ms = Scheduler::NowMs() - scan_t0;
    SerialLogger::Log("[kldb] pci scan: ");
    SerialLogger::LogDec(g_dev_count);
    SerialLogger::Log(" devices, ");
    SerialLogger::LogDec((int)scan_ms);
    SerialLogger::Log(" ms (buses 0-");
    SerialLogger::LogDec((int)SCAN_BUS_LIMIT - 1);
    SerialLogger::Log(")\r\n");
    __atomic_store_n(&g_scan_state, 2, __ATOMIC_RELEASE);
}

// match a vendor/device against the cache, skipping the first `skip` hits so a
// container can enumerate multiple identical NICs. a match GRANTS it. (satoru)
int64_t op_pci_find(uint64_t as, int pid, uint32_t vendor, uint32_t device,
                    uint32_t skip, const char* proc_name) {
    g_find_calls++;
    if (vendor == 0 || vendor == 0xFFFF) return KLD_EINVAL;
    scan_bus_once();

    // choose the device. a container the auto matcher launched for one
    // adapter (take=vvvv:dddd@bdf, or the matched device of any auto family)
    // may only ever be granted that exact bdf: two identical adapters (the
    // native vga and a bochs-display share 1234:1111) must never make the
    // second one, or the desktop's own, reachable. other launch paths keep the
    // first match after `skip`. (satoru)
    Container* pc = find_container(as, false, -1);
    int chosen = -1;
    if (pc && pc->pref_bdf_valid) {
        if (skip != 0) return KLD_ENODEV;
        for (int i = 0; i < g_dev_count; i++) {
            if (g_devs[i].bdf != pc->pref_bdf) continue;
            if (g_devs[i].vendor != (uint16_t)vendor) continue;
            if (g_devs[i].device != (uint16_t)device) continue;
            chosen = i;
            break;
        }
    } else {
        uint32_t seen = 0;
        for (int i = 0; i < g_dev_count; i++) {
            if (g_devs[i].vendor != (uint16_t)vendor) continue;
            if (g_devs[i].device != (uint16_t)device) continue;
            if (seen++ < skip) continue;
            chosen = i;
            break;
        }
    }
    if (chosen < 0) return KLD_ENODEV;
    {
        int i = chosen;
        if (granted_elsewhere(as, g_devs[i].bdf)) return KLD_EBUSY;
        if (!add_grant(as, pid, g_devs[i].bdf)) return KLD_ENOSPC;
        Grant* gr = find_grant(as, g_devs[i].bdf);
        Container* c = find_container(as, true, pid);
        // (satoru) every start path must name its family: the kernel runners
        // set it from /usr/bin/kld-<family>, and a container started by
        // execve (the kld bench) is named by the loader from the same path.
        // this is the kernel's record, never something the container asks
        // for. (satoru)
        if (c && !c->family[0] && proc_name)
            family_from_path(proc_name, c->family, (int)sizeof(c->family));
        // (satoru) vt-d: the first grant may turn on translation and put this
        // device in the container's own second-level domain. a failure leaves
        // today's dma behaviour; kld_iommu logs why.
        const char* fam0 = (c && c->family[0]) ? c->family : nullptr;
        if (!KldModel::IsModel(g_devs[i].bdf)) {
            // review g2: a bar_policy family (its device is mapped with the
            // display/gt write window and can write ggtt ptes) is granted its
            // device ONLY with its vt-d domain active. no domain = fail closed,
            // the grant is withdrawn. other families keep today's behaviour.
            // (satoru)
            // review g1 (turn 6): a bar_policy family's real device is handed
            // out only with kurono.wp=1 (CR0.WP on every cpu), so a kernel
            // copy-out into one of its read-only bar pages faults and fails the
            // syscall instead of writing through. (satoru)
            if (fam0 && (KldAuto::FamilyFlags(fam0) & KldAuto::FAMILY_BAR_POLICY) && !SMP::WpOn()) {
                log_bdf("[kldb] refused ", g_devs[i].bdf);
                SerialLogger::Log(": bar_policy family runs only with kurono.wp=1\r\n");
                remove_grant(as, g_devs[i].bdf);
                return KLD_ENODEV;
            }
            bool on = KldIommu::EnsureOn(g_devs[i].bdf);
            if (fam0 && (KldAuto::FamilyFlags(fam0) & KldAuto::FAMILY_BAR_POLICY) && !on) {
                log_bdf("[kldb] refused ", g_devs[i].bdf);
                SerialLogger::Log(": bar_policy family needs an active vt-d domain and EnsureOn failed\r\n");
                remove_grant(as, g_devs[i].bdf);
                return KLD_ENODEV;
            }
            if (on && KldIommu::Attach(as, fam0, g_devs[i].bdf) && gr)
                gr->dma_isolated = true;
        } else if (gr) {
            gr->dma_isolated = true;   // the model device does no dma (satoru)
        }
        if (gr) {
            // (satoru) the MSI-X table page exception is a property of the
            // CALLER'S driver family: the runner recorded it from the
            // /usr/bin/kld-<family> path, kld/families/<family>.manifest
            // declares needs_msix_table_page, and the device must be in that
            // same family's own id table. a container of another family, an
            // unknown family, or an id outside the table never gets the flag.
            const char* fam = (c && c->family[0]) ? c->family : nullptr;
            // legacy vga is a manifest property of the CALLER's family and the
            // device must be in that family's own id table. (satoru)
            if (fam && (KldAuto::FamilyFlags(fam) & KldAuto::FAMILY_LEGACY_VGA)) {
                bool fam_need = false;
                if (KldAuto::FamilyMatchDeviceByName(fam, g_devs[i].vendor,
                                                     g_devs[i].device,
                                                     g_devs[i].subvendor,
                                                     g_devs[i].subdevice,
                                                     g_devs[i].class_code,
                                                     &fam_need))
                    gr->legacy_vga = true;
            }
            if (MsixTableFlagForGrant(fam, g_devs[i].vendor, g_devs[i].device,
                                      g_devs[i].subvendor, g_devs[i].subdevice,
                                      g_devs[i].class_code)) {
                gr->allow_msix_table_page = true;
                SerialLogger::Log("[kldb] family ");
                SerialLogger::Log(fam);
                SerialLogger::Log(" needs the bar page holding the msi-x table\r\n");
            } else if (KldAuto::TablePresent()) {
                const char* any = nullptr;
                bool any_need = false;
                if (!KldAuto::MatchDevice(g_devs[i].vendor, g_devs[i].device,
                                          g_devs[i].subvendor, g_devs[i].subdevice,
                                          g_devs[i].class_code, &any, &any_need))
                    log_nofam_once(g_devs[i].vendor, g_devs[i].device);
            }
        }
        log_bdf("[kldb] granted ", g_devs[i].bdf);
        SerialLogger::Log(" to pid ");
        SerialLogger::LogDec(pid);
        SerialLogger::Log(" (vendor/device matched)\r\n");
        return (int64_t)g_devs[i].bdf;
    }
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

int msix_page_kind(uint32_t bdf, uint64_t page);
bool msix_overlaps(uint32_t bdf, uint64_t phys, uint64_t bytes);

// kurono.kld.bar=ro|window (d4): for a family whose manifest declares
// bar_policy 1, bar pages are mapped read-only (ro), or read-only outside the
// manifest's bar_window ranges (window). a write to such a page faults the
// container (the #PF path refuses writes to read-only broker pages). every
// other family, and every boot without the flag, maps bars writable. (satoru)
int g_bar_mode;   // 0 rw, 1 ro, 2 window (satoru)

uint64_t parse_hex(const char** pp) {
    const char* p = *pp;
    uint64_t v = 0;
    for (;;) {
        char ch = *p;
        int h = (ch >= '0' && ch <= '9') ? ch - '0'
              : (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10
              : (ch >= 'A' && ch <= 'F') ? ch - 'A' + 10 : -1;
        if (h < 0) break;
        v = (v << 4) | (uint64_t)h;
        p++;
    }
    *pp = p;
    return v;
}

// window string from the manifest: "<bar>:<lo>-<hi>,<lo>-<hi>;<bar>:..."
// (hex, inclusive). (satoru)
bool in_bar_window(const char* w, uint32_t idx, uint64_t off) {
    const char* p = w;
    while (p && *p) {
        uint64_t bar = parse_hex(&p);
        if (*p++ != ':') return false;
        for (;;) {
            uint64_t lo = parse_hex(&p);
            if (*p++ != '-') return false;
            uint64_t hi = parse_hex(&p);
            if (bar == idx && off >= lo && off <= hi) return true;
            if (*p == ',') { p++; continue; }
            break;
        }
        if (*p == ';') p++;
        else break;
    }
    return false;
}

int64_t op_map_bar(uint64_t as, uint32_t bdf, uint32_t idx, uint64_t va, uint64_t bytes) {
    if (!bdf_valid(bdf) || idx > 5) return KLD_EINVAL;
    Grant* grant = find_grant(as, bdf);
    if (!grant) return KLD_EPERM;
    bool is64 = false;
    uint64_t base = bar_base(bdf, idx, &is64);
    if (!base) return KLD_ENODEV;
    // the model device's bars are kernel ram: page frames come from the
    // model, mapped write-back (ram must not be aliased uncached). (satoru)
    bool model = KldModel::IsModel(bdf);
    uint64_t size = bar_size(bdf, idx);
    if (bytes == 0 || bytes > size) bytes = size;
    uint64_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    Container* c = find_container(as, true, -1);
    if (!c) return KLD_ENOSPC;
    MapRec* m = nullptr;
    for (int i = 0; i < MAX_MAPS; i++) if (!c->maps[i].used) { m = &c->maps[i]; break; }
    if (!m) return KLD_ENOSPC;
    if (!va_range_free(as, va, pages)) return KLD_EBUSY;

    // (satoru) a device whose matched family does not declare
    // needs_msix_table_page keeps the original whole-BAR refusal: if the
    // range touches the MSI-X table OR the PBA, nothing is mapped or
    // retained. the BE200's iwlwifi family may map the TABLE page because its
    // CSR_MSIX registers share that page; PBA-only pages are skipped (left
    // absent) and are never retained.
    if (!grant->allow_msix_table_page &&
        msix_overlaps(bdf, base, pages * PAGE_SIZE)) {
        log_bdf("[kldb] refused MAP_BAR over the msi-x table/pba of ", bdf);
        SerialLogger::Log("\r\n");
        return KLD_EPERM;
    }

    uint64_t fl = PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_PCD | PTE_PWT | PTE_NX;
    if (model) fl = PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_NX;
    const char* fam = c->family[0] ? c->family : nullptr;
    // with kurono.kld.bar= set the policy fails closed: a caller whose family
    // properties cannot be read (no table, unknown or empty family) gets no
    // bar at all. (satoru)
    if (g_bar_mode && (!fam || !KldAuto::FamilyKnown(fam))) {
        log_bdf("[kldb] refused MAP_BAR of ", bdf);
        SerialLogger::Log(": kurono.kld.bar is set and the caller's family properties are unknown\r\n");
        return KLD_EPERM;
    }
    int mode = (g_bar_mode &&
                (KldAuto::FamilyFlags(fam) & KldAuto::FAMILY_BAR_POLICY)) ? g_bar_mode : 0;
    const char* window = mode == 2 ? KldAuto::FamilyBarWindow(fam) : nullptr;
    if (mode == 2 && !window) mode = 1;   // no window declared: read-only (satoru)
    uint64_t ro_pages = 0;

    // pass 1: classify and decide. nothing is mapped or retained yet, so a
    // refusal here is still side-effect free. (satoru)
    bool table_page = false;
    for (uint64_t i = 0; i < pages; i++) {
        int kind = msix_page_kind(bdf, base + i * PAGE_SIZE);
        if (kind == 2)
            continue;                       // pba-only page: never mapped
        if (kind == 1) {
            if (!grant->allow_msix_table_page) {
                log_bdf("[kldb] refused MAP_BAR over the msi-x table of ", bdf);
                SerialLogger::Log("\r\n");
                return KLD_EPERM;
            }
            table_page = true;
        }
    }
    if (table_page) {
        log_bdf("[kldb] allowed bar page holding the msi-x table AND registers of ", bdf);
        SerialLogger::Log(" (family property; container may rewrite entries)\r\n");
    }

    // pass 2: retain and map. if map_pages fails, undo exactly what this pass
    // did: unmap the pages it installed and drop both refs it took for each,
    // including the page that failed. skipped pages are never touched. (satoru)
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys = base + i * PAGE_SIZE;
        if (msix_page_kind(bdf, phys) == 2)
            continue;
        if (model) {
            phys = KldModel::BarPagePhys(idx, i);
            if (!phys) continue;   // unbacked model page: left absent (satoru)
        }
        uint64_t pfl = fl;
        // the ggtt (upper half of bar0) is part of the window only for a
        // dma-isolated grant: ptes aim the device at bus addresses. (satoru)
        bool gsm = mode == 2 && idx == 0 && i * PAGE_SIZE >= (8ull << 20) && grant->dma_isolated;
        if (mode == 1 || (mode == 2 && !gsm && !in_bar_window(window, idx, i * PAGE_SIZE))) {
            pfl &= ~(uint64_t)PTE_WRITABLE;
            ro_pages++;
        }
        // mmio frames are retained twice (teardown only decrements); a model
        // ram frame is owned by the model, so the mapping holds exactly one
        // ref that the teardown drops (review f6). (satoru)
        PMM::RetainFrame(phys);
        if (!model) PMM::RetainFrame(phys);
        if (!map_pages(as, va + i * PAGE_SIZE, phys, 1, pfl)) {
            // (satoru) MMIO frames are retained twice so a later teardown
            // FreeFrame can only decrement. never FreeFrame here: a leaked
            // ref on a device frame is harmless, while freeing one would put
            // device memory on the PMM free list. just unmap what this pass
            // installed and leave the refs.
            for (uint64_t j = 0; j < i; j++) {
                uint64_t prev = base + j * PAGE_SIZE;
                if (msix_page_kind(bdf, prev) == 2)
                    continue;
                if (model && !KldModel::BarPagePhys(idx, j))
                    continue;
                unmap_pages(as, va + j * PAGE_SIZE, 1);
            }
            return KLD_ENOMEM;
        }
    }
    m->used = true;
    m->plat = false;
    m->va = va;
    m->phys = base;
    m->pages = (uint32_t)pages;
    log_bdf("[kldb] mapped bar of ", bdf);
    SerialLogger::Log(" idx="); SerialLogger::LogDec((int)idx);
    SerialLogger::Log(" phys=0x"); SerialLogger::LogHex64(base);
    SerialLogger::Log(" pages="); SerialLogger::LogDec((int)pages);
    SerialLogger::Log(model ? " (model ram,wb,nx)" : " (uc,nx)");
    if (mode) {
        SerialLogger::Log(mode == 1 ? " policy=ro" : " policy=window");
        SerialLogger::Log(" ro_pages=");
        SerialLogger::LogDec((int)ro_pages);
    }
    SerialLogger::Log("\r\n");
    return KLD_OK;
}

uint32_t map_type(uint64_t phys);
bool is_display_class(uint32_t bdf);
uint32_t g_opregion_refused;

void opregion_refuse(const char* why) {
    if (g_opregion_refused++ < 4) {
        SerialLogger::Log("[kldb] OPREGION_MAP refused: ");
        SerialLogger::Log(why);
        SerialLogger::Log("\r\n");
    }
}

// the graphics opregion (d4): config 0xFC (asls) of the caller's own granted
// display device names a firmware table. the container never gets firmware
// memory: the broker validates the source (signature, reserved/acpi memory or
// the model's table, rvda/rvds bounds) and maps a PRIVATE COPY in
// container-owned frames, read-write, as a DMA record of the container (its
// budget, its release path). the driver's mailbox writes land in the copy and
// reach no firmware. only for a family whose manifest says opregion 1.
// layout of the copy: the 8 KiB region, then the rvda vbt if the header has
// one (rvda rewritten to point right behind the region). returns
// (copy bytes << 12) | offset of the region in the first page. (satoru)
constexpr uint64_t OPREGION_BYTES = 8192;
constexpr uint64_t OPREGION_RVDS_MAX = 64 * 1024;

// overflow-safe (review f1): an empty, wrapping or partly unmapped range is
// refused. (satoru)
bool opregion_src_ok(uint64_t phys, uint64_t bytes) {
    if (!bytes || phys + bytes < phys || phys + bytes + 0xFFF < phys + bytes) return false;
    if (KldModel::OwnsOpRegion(phys, bytes)) return true;
    uint64_t first = phys & ~0xFFFull;
    uint64_t end = (phys + bytes + 0xFFF) & ~0xFFFull;
    for (uint64_t p = first; p < end; p += PAGE_SIZE) {
        uint32_t t = map_type(p);
        if (t != 2 && t != 3 && t != 4) return false;
    }
    return true;
}

int64_t op_opregion_map(uint64_t as, uint32_t bdf, uint64_t va) {
    if (!bdf_valid(bdf)) return KLD_EINVAL;
    if (!find_grant(as, bdf)) return KLD_EPERM;
    Container* c = find_container(as, false, -1);
    const char* fam = (c && c->family[0]) ? c->family : nullptr;
    if (!fam || !(KldAuto::FamilyFlags(fam) & KldAuto::FAMILY_OPREGION)) {
        opregion_refuse("family manifest has no opregion 1");
        return KLD_EPERM;
    }
    if (!is_display_class(bdf)) { opregion_refuse("not a display device"); return KLD_EPERM; }
    uint64_t asls = cfg_read(bdf, 0xFC, 4);
    if (!asls) return KLD_ENODEV;
    if (!opregion_src_ok(asls, OPREGION_BYTES)) {
        opregion_refuse("asls is not reserved/acpi memory");
        return KLD_EPERM;
    }
    const uint8_t* src = (const uint8_t*)PMM::PhysToVirt(asls);
    const char* sig = "IntelGraphicsMem";
    for (int i = 0; i < 16; i++)
        if (!src || src[i] != (uint8_t)sig[i]) { opregion_refuse("bad signature"); return KLD_ENODEV; }

    // rvda: opregion >= 2.0, asle mailbox present, rvda and rvds non-zero.
    // 2.0 = physical address, 2.1+ = offset from asls (i915's rules). (satoru)
    uint32_t over = (uint32_t)src[0x14] | ((uint32_t)src[0x15] << 8) |
                    ((uint32_t)src[0x16] << 16) | ((uint32_t)src[0x17] << 24);
    uint32_t mboxes = (uint32_t)src[0x58] | ((uint32_t)src[0x59] << 8);
    uint32_t major = over >> 24, minor = (over >> 16) & 0xFF;
    uint64_t rvda = 0, rvds = 0, rvda_phys = 0;
    for (int i = 0; i < 8; i++) rvda |= (uint64_t)src[0x3BA + i] << (8 * i);
    for (int i = 0; i < 4; i++) rvds |= (uint64_t)src[0x3C2 + i] << (8 * i);
    bool has_rvda = major >= 2 && (mboxes & 0x4) && rvda && rvds;
    bool rvda_rel = major > 2 || (major == 2 && minor >= 1);
    if (has_rvda) {
        if (rvds > OPREGION_RVDS_MAX) { opregion_refuse("rvds above 64 KiB"); return KLD_EPERM; }
        if (rvda_rel && rvda < OPREGION_BYTES) { opregion_refuse("relative rvda inside the region"); return KLD_EPERM; }
        if (rvda_rel && asls + rvda < asls) { opregion_refuse("rvda wraps"); return KLD_EPERM; }
        rvda_phys = rvda_rel ? asls + rvda : rvda;
        if (!opregion_src_ok(rvda_phys, rvds)) {
            opregion_refuse("rvda is not reserved/acpi memory");
            return KLD_EPERM;
        }
    }
    uint64_t copy_bytes = OPREGION_BYTES + (has_rvda ? rvds : 0);
    uint64_t off = asls & 0xFFFull;
    uint64_t pages = (off + copy_bytes + 0xFFF) / PAGE_SIZE;

    if (c->dma_pages + pages > c->dma_limit_pages) return KLD_ENOMEM;
    DmaRec* r = nullptr;
    for (int i = 0; i < MAX_DMA; i++) if (!c->dma[i].used) { r = &c->dma[i]; break; }
    if (!r) return KLD_ENOSPC;
    if (!va_range_free(as, va, pages)) return KLD_EBUSY;
    uint64_t phys = alloc_owned(pages, 52);
    if (!phys) return KLD_ENOMEM;
    uint8_t* dst = (uint8_t*)PMM::PhysToVirt(phys);
    for (uint64_t i = 0; i < pages * PAGE_SIZE; i++) dst[i] = 0;
    for (uint64_t i = 0; i < OPREGION_BYTES; i++) dst[off + i] = src[i];
    if (has_rvda) {
        const uint8_t* rv = (const uint8_t*)PMM::PhysToVirt(rvda_phys);
        for (uint64_t i = 0; i < rvds; i++) dst[off + OPREGION_BYTES + i] = rv[i];
        uint64_t nr = rvda_rel ? OPREGION_BYTES : asls + OPREGION_BYTES;
        for (int i = 0; i < 8; i++) dst[off + 0x3BA + i] = (uint8_t)(nr >> (8 * i));
    }
    uint64_t fl = PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_NX;
    if (!map_pages(as, va, phys, pages, fl)) {
        free_after_tlb_proof(phys, pages, 2);
        return KLD_ENOMEM;
    }
    r->used = true; r->va = va; r->phys = phys; r->pages = (uint32_t)pages;
    r->freeing = false;
    c->dma_pages += (uint32_t)pages;
    log_bdf("[kldb] opregion of ", bdf);
    SerialLogger::Log(" copied (private, rw) from phys=0x");
    SerialLogger::LogHex64(asls);
    SerialLogger::Log(" bytes=");
    SerialLogger::LogDec((int)copy_bytes);
    SerialLogger::Log(has_rvda ? " rvda=copied" : " rvda=none");
    SerialLogger::Log(KldModel::OwnsOpRegion(asls, OPREGION_BYTES) ? " (model)\r\n" : "\r\n");
    return (int64_t)((copy_bytes << 12) | off);
}

// the host bridge 00:00.0, read-only, for a display family whose manifest
// says host_bridge_read 1 and that holds a display grant, offsets from a
// fixed allowlist (i915 reads the bridge's mchbar register; it never needs a
// write on a booted machine).
// with the model device the bridge is the model's. (satoru)
#include "kld_gt_allow.inc"

bool gt_reg_allowed(uint32_t off) {
    for (unsigned i = 0; i < sizeof(kGtAllow) / sizeof(kGtAllow[0]); i++)
        if (kGtAllow[i] == (off & ~3u)) return true;
    return false;
}

uint32_t g_gt_refused;

// review g3: a gt register write of a bar_policy display container (its gt
// pages are mapped read-only, so the container asks the broker). only the
// registers on the measured allowlist, only the device the caller is granted,
// and only to the model's gt page or (on hardware, later) the real bar. the
// write lands in the model's bar0 ram so the register model still sees it.
// pcode: only the one command observed (data in 0x138128, command 0x1a in
// 0x138124). cost: one syscall per gt write. (satoru)
int64_t op_gt_write(uint64_t as, uint32_t off, uint32_t width, uint64_t val) {
    if (width != 1 && width != 2 && width != 4 && width != 8) return KLD_EINVAL;
    Container* c = find_container(as, false, -1);
    const char* fam = (c && c->family[0]) ? c->family : nullptr;
    if (!fam || !(KldAuto::FamilyFlags(fam) & KldAuto::FAMILY_BAR_POLICY)) return KLD_EPERM;
    uint32_t bdf = 0; bool have = false;
    for (int i = 0; i < MAX_GRANTS; i++)
        if (grant_live(g_grants[i], as) && is_display_class(g_grants[i].bdf)) { bdf = g_grants[i].bdf; have = true; break; }
    if (!have) return KLD_EPERM;
    if (!gt_reg_allowed(off) ||
        (off == 0x138124 && (val & 0xff) != 0x1a)) {
        if (g_gt_refused++ < 8) {
            SerialLogger::Log("[kldb] GT_WRITE refused off=0x");
            SerialLogger::LogHex(off);
            SerialLogger::Log(" (not on the display path allowlist)\r\n");
        }
        return KLD_EPERM;
    }
    if (KldModel::IsModel(bdf)) {
        uint64_t phys = KldModel::BarPagePhys(0, off / PAGE_SIZE);
        if (!phys) return KLD_ENODEV;
        uint8_t* p = (uint8_t*)PMM::PhysToVirt(phys) + (off & 0xFFF);
        if (width == 1) *p = (uint8_t)val;
        else if (width == 2) *(uint16_t*)p = (uint16_t)val;
        else if (width == 4) *(uint32_t*)p = (uint32_t)val;
        else *(uint64_t*)p = val;
        return KLD_OK;
    }
    return KLD_ENODEV;   // hardware path lands in d4.5 (satoru)
}

int64_t op_host_cfg_read(uint64_t as, uint32_t off, uint32_t width) {
    if (off > 0xFF) return KLD_EINVAL;
    if (width != 1 && width != 2 && width != 4) return KLD_EINVAL;
    if (off & (width - 1)) return KLD_EINVAL;
    // what i915 reads from 00:00.0: vendor/device (0x00-0x03), mchbar
    // (MCHBAR_I965 0x48-0x4f, intel_gmch_bar_setup), gmch control
    // (0x50-0x53, intel_gmch_vga_set_state). nothing else. (satoru)
    bool listed = (off + width <= 0x04) ||
                  (off >= 0x48 && off + width <= 0x54);
    if (!listed) return KLD_EPERM;
    Container* c = find_container(as, false, -1);
    const char* fam = (c && c->family[0]) ? c->family : nullptr;
    if (!fam || !(KldAuto::FamilyFlags(fam) & KldAuto::FAMILY_HOST_BRIDGE_READ))
        return KLD_EPERM;
    bool display = false, model = false;
    for (int i = 0; i < MAX_GRANTS; i++) {
        if (!grant_live(g_grants[i], as)) continue;
        if (is_display_class(g_grants[i].bdf)) display = true;
        if (KldModel::IsModel(g_grants[i].bdf)) model = true;
    }
    if (!display) return KLD_EPERM;
    if (model) return (int64_t)KldModel::HostCfgRead(off, width);
    return (int64_t)cfg_read(PackBdf(0, 0, 0), off, width);
}

int64_t op_dma_alloc(uint64_t as, int pid, uint64_t va, uint64_t pages, uint32_t bits) {
    if (bits < 24 || bits > 64) return KLD_EINVAL;
    if (pages == 0 || pages > 4096) return KLD_EINVAL;
    Container* c = find_container(as, false, pid);
    if (!c) return KLD_EPERM;   // only a container holding a grant may allocate (satoru)
    // per-container cap, set at registration (default 64 MiB). (satoru)
    if (c->dma_pages + pages > c->dma_limit_pages) return KLD_ENOMEM;
    DmaRec* r = nullptr;
    for (int i = 0; i < MAX_DMA; i++) if (!c->dma[i].used) { r = &c->dma[i]; break; }
    if (!r) return KLD_ENOSPC;
    if (!va_range_free(as, va, pages)) return KLD_EBUSY;
    uint64_t phys = alloc_owned(pages, bits);
    if (!phys) return KLD_ENOMEM;
    // x86 dma is cache coherent: write-back is correct for coherent memory. (satoru)
    uint64_t fl = PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_NX;
    if (!map_pages(as, va, phys, pages, fl)) {
        // drop the retain and the allocation; map_pages may have installed
        // and cleared some ptes, so on proof. (satoru)
        free_after_tlb_proof(phys, pages, 2);
        return KLD_ENOMEM;
    }
    // (satoru) vt-d: every frame the device can reach is mapped in the
    // container's domain for exactly this allocation's lifetime. failure here
    // fails the allocation instead of leaving the device able to fault on it.
    if (!KldIommu::MapDma(as, phys, (uint32_t)pages)) {
        unmap_pages(as, va, pages);
        free_after_tlb_proof(phys, pages, 2);   // mapping + allocation ref (satoru)
        return KLD_ENOMEM;
    }
    r->used = true; r->va = va; r->phys = phys; r->pages = (uint32_t)pages;
    r->freeing = false;
    c->dma_pages += (uint32_t)pages;
    return (int64_t)phys;
}

bool alias_holds_locked(Container* c, uint64_t phys, uint32_t pages);

int64_t op_dma_free(uint64_t as, uint64_t va) {
    Container* c = find_container(as, false, -1);
    if (!c) return KLD_EPERM;
    for (int i = 0; i < MAX_DMA; i++) {
        DmaRec* r = &c->dma[i];
        if (!r->used || r->va != va) continue;
        {
            // unalias first; once freeing is set no alias can take these
            // frames (op_dma_alias looks the source up under this lock). (satoru)
            SpinLockGuard g(g_alias_lock);
            if (r->freeing || alias_holds_locked(c, r->phys, r->pages)) return KLD_EBUSY;
            r->freeing = true;
        }
        KldIommu::UnmapDma(as, r->phys, r->pages);
        unmap_pages(as, r->va, r->pages);
        free_after_tlb_proof(r->phys, r->pages, 2);   // mapping + broker ref (satoru)
        c->dma_pages -= r->pages;
        r->used = false;
        return KLD_OK;
    }
    return KLD_EINVAL;
}

// ── legacy vga port i/o: display-class grant, family manifest, sole decoder ─
// a ring-3 driver cannot execute in/out. the VGA block is one global legacy
// resource addressed to whichever adapter decodes it, so only a display-class
// grant whose family asked (legacy_vga=1) may touch it, AND the device must be
// the machine's only io-decoding display adapter, or the boot flag must force
// it. otherwise the access is not routed: out is dropped, in reads 0xff, and
// one line per container says why. (satoru)
int64_t op_port_in(uint64_t as, uint64_t port, uint64_t width);
int64_t op_port_out(uint64_t as, uint64_t port, uint64_t width, uint64_t val);

uint32_t cfg_class_code(uint32_t bdf)
{
    return (uint32_t)(cfg_read(bdf, 0x08, 4) >> 8);
}

bool is_display_class(uint32_t bdf)
{
    return ((cfg_class_code(bdf) >> 16) & 0xFFu) == 0x03u;
}

Grant* find_legacy_grant(uint64_t as)
{
    for (int i = 0; i < MAX_GRANTS; i++) {
        Grant* g = &g_grants[i];
        if (grant_live(*g, as) && g->legacy_vga && is_display_class(g->bdf))
            return g;
    }
    return nullptr;
}

// true when `bdf` is the only display adapter in the machine that decodes the
// legacy io ports. scan_bus_once() has populated class codes; the command
// register read is live because a driver may have enabled io after the scan.
bool owns_legacy_vga(uint32_t bdf)
{
    scan_bus_once();
    int io_display = 0;
    uint32_t owner = 0;
    for (int i = 0; i < g_dev_count; i++) {
        if (((g_devs[i].class_code >> 16) & 0xFFu) != 0x03u)
            continue;
        if (!(cfg_read(g_devs[i].bdf, 0x04, 2) & 0x1))
            continue;
        io_display++;
        owner = g_devs[i].bdf;
    }
    return io_display == 1 && owner == bdf;
}

bool vga_port_ok(uint64_t port, uint64_t width)
{
    if (width != 1 && width != 2 && width != 4)
        return false;
    if (port < 0x3b0 || port > 0x3df)
        return false;
    return port + width - 1 <= 0x3df;
}

int64_t legacy_vga_prepare(uint64_t as)
{
    Grant* g = find_legacy_grant(as);
    if (!g)
        return KLD_EPERM;
    if (g_legacy_vga_forced || owns_legacy_vga(g->bdf))
        return KLD_OK;
    Container* c = find_container(as, false, -1);
    if (c && !c->legacy_vga_warned) {
        c->legacy_vga_warned = true;
        SerialLogger::Log("[kldb] legacy vga ports not routed to ");
        SerialLogger::Log(c->family[0] ? c->family : "?");
        SerialLogger::Log(": another display adapter decodes them\r\n");
    }
    return KLD_ENOROUTE;
}

int64_t op_port_in(uint64_t as, uint64_t port, uint64_t width)
{
    int64_t prep = legacy_vga_prepare(as);
    if (prep < 0) return prep;
    if (!vga_port_ok(port, width)) return KLD_EINVAL;
    switch (width) {
    case 1: return inb((unsigned short)port);
    case 2: return inw((unsigned short)port);
    default: return (int64_t)(uint32_t)inl((unsigned short)port);
    }
}

int64_t op_port_out(uint64_t as, uint64_t port, uint64_t width, uint64_t val)
{
    int64_t prep = legacy_vga_prepare(as);
    if (prep < 0) return prep;
    if (!vga_port_ok(port, width)) return KLD_EINVAL;
    switch (width) {
    case 1: outb((unsigned char)val, (unsigned short)port); break;
    case 2: outw((unsigned short)val, (unsigned short)port); break;
    default: outl((unsigned int)val, (unsigned short)port); break;
    }
    return KLD_OK;
}

// ── display instance state machine (D2.8/D2.9) ─────────────────────────────
// One broker record owns the ring+scanout. States:
//   ALLOCATED -> ATTACHED -> PUBLISHED -> DYING -> RELEASED
// The record stores the counts it allocated. The only fact about the address
// space is as_dead, set from the teardown path before the PML4 is freed (or by
// the exit path, which runs after the teardown). The final free unmaps and
// returns the mapping ref only when the AS is alive at that moment; otherwise
// the teardown already dropped that ref. Only disp_prepare_free_locked() +
// disp_run_free() free, keyed by id, and the prepare refuses ATTACHED and
// PUBLISHED. (satoru)
enum DispState : uint32_t {
    DISP_FREE = 0,
    DISP_ALLOCATED,
    DISP_ATTACHED,
    DISP_PUBLISHED,
    DISP_DYING,
    DISP_RELEASED,
};

struct DispInst {
    bool     used;
    uint32_t id;
    uint32_t state;
    uint64_t as;
    int      pid;
    uint64_t ring_phys, ring_va;
    uint32_t ring_pages;
    uint64_t scan_phys, scan_va;
    uint32_t scan_pages;
    bool     as_dead;
    bool     freeing;   // the final free owns the frames until this clears (satoru)
};
constexpr int MAX_DISP_INST = 8;
DispInst g_disp_inst[MAX_DISP_INST];
uint32_t g_disp_next_id = 1;
Spinlock g_disp_lock;
bool g_disp_full_logged;

static DispInst* disp_find_locked(uint32_t id)
{
    if (!id) return nullptr;
    for (int i = 0; i < MAX_DISP_INST; i++)
        if (g_disp_inst[i].used && g_disp_inst[i].id == id)
            return &g_disp_inst[i];
    return nullptr;
}

static DispInst* disp_alloc_locked(void)
{
    for (int i = 0; i < MAX_DISP_INST; i++)
        if (!g_disp_inst[i].used)
            return &g_disp_inst[i];
    return nullptr;
}

static bool disp_cas_locked(DispInst* r, uint32_t from, uint32_t to,
                            const char* what)
{
    if (r->state != from) {
        SerialLogger::Log("[kldb] display illegal transition ");
        SerialLogger::Log(what);
        SerialLogger::Log(" (state ");
        SerialLogger::LogDec((int)r->state);
        SerialLogger::Log(")\r\n");
        return false;
    }
    r->state = to;
    return true;
}

// the frame work of the final free runs outside g_disp_lock: walking 2026
// leaves and dropping their refs with interrupts off was a long lock hold.
// the record stays in the table with state RELEASED and freeing set, so a
// second release is refused and the vmm teardown hook waits for this free
// before it frees the same leaves. (satoru)
struct DispFree {
    uint32_t id;
    uint64_t as;
    uint64_t ring_phys, ring_va;
    uint32_t ring_pages;
    uint64_t scan_phys, scan_va;
    uint32_t scan_pages;
    bool     as_dead;
};

static bool disp_prepare_free_locked(DispInst* r, DispFree* d)
{
    if (!r || !r->used)
        return false;
    if (r->state == DISP_RELEASED || r->freeing) {
        SerialLogger::Log("[kldb] display release refused twice (id ");
        SerialLogger::LogDec((int)r->id);
        SerialLogger::Log(")\r\n");
        return false;
    }
    if (r->state == DISP_ATTACHED || r->state == DISP_PUBLISHED) {
        SerialLogger::Log("[kldb] display release refused while attached\r\n");
        return false;
    }
    // every frame, not just the first: a path that lost one mapping ref must be
    // caught before the final free drops it a second time. (satoru)
    for (uint32_t i = 0; i < r->ring_pages; i++) {
        if (!PMM::GetFrameRefCount(r->ring_phys + (uint64_t)i * PAGE_SIZE)) {
            SerialLogger::Log("[kldb] display final-free: ring frame already free, refused\r\n");
            return false;
        }
    }
    for (uint32_t i = 0; i < r->scan_pages; i++) {
        if (!PMM::GetFrameRefCount(r->scan_phys + (uint64_t)i * PAGE_SIZE)) {
            SerialLogger::Log("[kldb] display final-free: scanout frame already free, refused\r\n");
            return false;
        }
    }
    d->id = r->id;
    d->as = r->as;
    d->ring_phys = r->ring_phys; d->ring_va = r->ring_va;
    d->ring_pages = r->ring_pages;
    d->scan_phys = r->scan_phys; d->scan_va = r->scan_va;
    d->scan_pages = r->scan_pages;
    d->as_dead = r->as_dead;
    r->state = DISP_RELEASED;
    r->freeing = true;
    return true;
}

// one batch of the final free. as_dead is re-read under g_disp_lock per batch:
// the vmm teardown hook sets it and then waits for freeing, so once it is true
// the teardown owns the mapping refs and this side must not touch the leaves.
// design B: the free runs outside the lock, the hook has no timeout path.
// (satoru)
static void disp_free_batch(const DispFree& d, uint64_t phys, uint64_t va,
                            uint32_t off, uint32_t n, bool* flushed)
{
    bool dead;
    {
        SpinLockGuard g(g_disp_lock);
        DispInst* r = disp_find_locked(d.id);
        dead = !r || r->as_dead;
    }
    bool warned = false;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t p = phys + (uint64_t)(off + i) * PAGE_SIZE;
        uint64_t v = va + (uint64_t)(off + i) * PAGE_SIZE;
        uint32_t rc = PMM::GetFrameRefCount(p);
        if (!dead) {
            uint64_t cur = KernelVMM::QueryMappingInAddressSpace(d.as, v);
            if (cur == p) {
                // the pte still maps this frame: unmap it first, whatever the
                // refcount says, then drop only the refs that exist. (satoru)
                KernelVMM::UnmapPageInAddressSpace(d.as, v, false);
                *flushed = true;
                if (rc >= 2) {
                    // the mapping ref and the allocation ref, after the
                    // batch's shootdown is proven (the free used to drop them
                    // before its own flush). (satoru)
                    free_after_tlb_proof(p, 1, 2);
                    continue;
                }
                if (!warned) {
                    warned = true;
                    SerialLogger::Log("[kld-warn] display final-free: refcount ");
                    SerialLogger::LogDec((int)rc);
                    SerialLogger::Log(" under a live pte\r\n");
                }
                if (rc) free_after_tlb_proof(p, 1, rc);   // the refs that exist (satoru)
                continue;
            } else if (!warned) {
                warned = true;
                SerialLogger::Log("[kldb] display final-free: frame not mapped, allocation ref only\r\n");
            }
        }
        if (rc) PMM::FreeFrame(p);              // the broker's allocation ref
    }
}

static void disp_free_range(const DispFree& d, uint64_t phys, uint64_t va,
                            uint32_t pages)
{
    const uint32_t BATCH = 256;
    bool flushed = false;
    int delay = KldDisplay::FreeDelayMsForTest();
    for (uint32_t off = 0; off < pages; off += BATCH) {
        uint32_t n = (pages - off > BATCH) ? BATCH : (pages - off);
        disp_free_batch(d, phys, va, off, n, &flushed);
        if (flushed) {
            SMP::BroadcastTlbFlush();
            flushed = false;
        }
        if (delay > 0 && off == 0 && pages > BATCH) {
            // rig: hold the free between two batches while the container's
            // address space is destroyed on another cpu. (satoru)
            Scheduler::SleepMs((uint32_t)delay);
        }
    }
}

static void disp_finish_free(const DispFree& d)
{
    {
        SpinLockGuard g(g_disp_lock);
        DispInst* r = disp_find_locked(d.id);
        if (r) {
            __atomic_store_n(&r->freeing, false, __ATOMIC_RELEASE);
            r->used = false;
        }
    }
    SerialLogger::Log("[kldb] display instance ");
    SerialLogger::LogDec((int)d.id);
    SerialLogger::Log(" released\r\n");
}

static void disp_run_free(const DispFree& d)
{
    // the rig delay needs the teardown to run on another cpu, which is only
    // possible while kls is not held by this free. production never drops it.
    // (satoru)
    int delay = KldDisplay::FreeDelayMsForTest();
    int kls = 0;
    if (delay > 0 && LinuxSyscall::KlsHeldHere())
        kls = LinuxSyscall::KlsDropAll();
    if (!d.as_dead) {
        disp_free_range(d, d.ring_phys, d.ring_va, d.ring_pages);
        disp_free_range(d, d.scan_phys, d.scan_va, d.scan_pages);
    } else {
        // the teardown already dropped the mapping refs; only ours remain. (satoru)
        free_owned(d.ring_phys, d.ring_pages);
        free_owned(d.scan_phys, d.scan_pages);
    }
    disp_finish_free(d);
    if (kls) LinuxSyscall::KlsRestore(kls);
}

int64_t dispint_attach(uint64_t as, int pid, uint64_t ring_va,
                       uint64_t scanout_va, uint32_t ring_pages,
                       uint32_t scanout_pages, uint32_t* id_out)
{
    if (!KldDisplay::Wanted())
        return KLD_ENODEV;
    // validate the counts BEFORE any allocation/mapping: a hostile count must
    // not allocate or unmap gigabytes. (satoru)
    if (ring_pages != KldDisplay::KLD_DISP_RING_PAGES ||
        scanout_pages != KldDisplay::KLD_DISP_SCANOUT_PAGES) {
        // the badcount rig proves the refusal allocates nothing: one line per
        // call with the pmm frame count, so the log alone shows it is flat. (satoru)
        static uint32_t s_badcount_logged;
        if (KldDisplay::BadCountTest() && s_badcount_logged < 100) {
            s_badcount_logged++;
            SerialLogger::Log("[klddisp] badcount refused used_mb=");
            SerialLogger::LogDec((int)((PMM::GetUsedFrames() * 4096ull) / (1024 * 1024)));
            SerialLogger::Log("\r\n");
        }
        return KLD_EINVAL;
    }
    Container* c = find_container(as, false, pid);
    if (!c) return KLD_EPERM;
    // only a display family (the manifest flag the legacy vga port ops also
    // require) with a display-class grant may run the display bridge. any
    // other container can PCI_FIND a display device but must not attach it.
    // (satoru)
    if (!c->family[0] || !(KldAuto::FamilyFlags(c->family) & KldAuto::FAMILY_DISPLAY)) {
        if (!c->legacy_vga_warned) {
            c->legacy_vga_warned = true;
            SerialLogger::Log("[kldb] display attach refused: not a display family\r\n");
        }
        return KLD_EPERM;
    }
    bool display_grant = false;
    for (int i = 0; i < MAX_GRANTS; i++)
        if (grant_live(g_grants[i], as) && is_display_class(g_grants[i].bdf))
            display_grant = true;
    if (!display_grant) {
        if (!c->legacy_vga_warned) {
            c->legacy_vga_warned = true;
            SerialLogger::Log("[kldb] display attach refused: no display-class grant\r\n");
        }
        return KLD_EPERM;
    }
    int64_t pre = KldDisplay::AttachPrecheck();
    if (pre < 0)
        return pre;
    if (!va_range_free(as, ring_va, ring_pages)) return KLD_EBUSY;
    if (!va_range_free(as, scanout_va, scanout_pages)) return KLD_EBUSY;
    uint64_t ring = alloc_owned(ring_pages, 52);
    if (!ring) return KLD_ENOMEM;
    uint64_t scan = alloc_owned(scanout_pages, 52);
    if (!scan) {
        free_owned(ring, ring_pages); free_owned(ring, ring_pages);
        return KLD_ENOMEM;
    }
    uint64_t rw = PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_NX;
    uint64_t ro = PTE_PRESENT | PTE_USER | PTE_NX;
    bool ring_mapped = map_pages(as, ring_va, ring, ring_pages, rw);
    bool scan_mapped = ring_mapped && map_pages(as, scanout_va, scan, scanout_pages, ro);
    if (!ring_mapped || !scan_mapped) {
        if (ring_mapped)
            unmap_pages(as, ring_va, ring_pages);
        free_after_tlb_proof(ring, ring_pages, 2);
        free_after_tlb_proof(scan, scanout_pages, 2);
        return KLD_ENOMEM;
    }
    uint32_t id = 0;
    {
        SpinLockGuard g(g_disp_lock);
        DispInst* r = disp_alloc_locked();
        if (!r) {
            if (!g_disp_full_logged) {
                g_disp_full_logged = true;
                SerialLogger::Log("[kldb] display instances exhausted\r\n");
            }
            unmap_pages(as, ring_va, ring_pages);
            unmap_pages(as, scanout_va, scanout_pages);
            free_after_tlb_proof(ring, ring_pages, 2);
            free_after_tlb_proof(scan, scanout_pages, 2);
            return KLD_ENOSPC;
        }
        id = g_disp_next_id++;
        if (!id) id = g_disp_next_id++;
        memset(r, 0, sizeof(*r));
        r->used = true;
        r->id = id;
        r->state = DISP_ALLOCATED;
        r->as = as;
        r->pid = pid;
        r->ring_phys = ring; r->ring_va = ring_va;
        r->ring_pages = ring_pages;
        r->scan_phys = scan; r->scan_va = scanout_va;
        r->scan_pages = scanout_pages;
        r->as_dead = false;
    }
    // ATTACHED before the bridge is visible to MODES: a concurrent MODES must
    // not see a live bridge on an ALLOCATED record. (satoru)
    {
        SpinLockGuard g(g_disp_lock);
        DispInst* r = disp_find_locked(id);
        if (!r || !disp_cas_locked(r, DISP_ALLOCATED, DISP_ATTACHED, "attach"))
            return KLD_EBUSY;
    }
    int64_t rc = KldDisplay::Attach(id, as, ring, scan, scanout_pages);
    if (rc < 0) {
        KldDisplay::DetachForRelease(id);
        DispFree d;
        bool run = false;
        {
            SpinLockGuard g(g_disp_lock);
            DispInst* r = disp_find_locked(id);
            if (r) {
                if (r->state == DISP_ATTACHED)
                    disp_cas_locked(r, DISP_ATTACHED, DISP_DYING, "attach-fail");
                run = disp_prepare_free_locked(r, &d);
            }
        }
        if (run) disp_run_free(d);
        return rc;
    }
    {
        SpinLockGuard g(g_disp_lock);
        c->disp_id = id;
    }
    if (id_out) *id_out = id;
    return KLD_OK;
}

void dispint_exit(uint32_t id, bool as_dead)
{
    bool call_detach = false;
    {
        SpinLockGuard g(g_disp_lock);
        DispInst* r = disp_find_locked(id);
        if (!r) return;
        if (as_dead) r->as_dead = true;   // sticky: never assigned false again (satoru)
        if (r->state == DISP_RELEASED) {
            if (!r->freeing) r->used = false;   // frames and mapping are already gone
            return;
        }
        if (r->state == DISP_ATTACHED || r->state == DISP_PUBLISHED ||
            r->state == DISP_ALLOCATED) {
            if (disp_cas_locked(r, r->state, DISP_DYING, "exit"))
                call_detach = true;
        } else if (r->state == DISP_DYING) {
            call_detach = true;
        }
    }
    if (call_detach) {
        // rig: widen the window between DYING and Detach so the gui can run in
        // it (detachdelay). one shot, test boots only. (satoru)
        static bool s_detach_delayed;
        int dms = KldDisplay::ExitDetachDelayMsForTest();
        if (!s_detach_delayed && dms > 0) {
            s_detach_delayed = true;
            Scheduler::SleepMs((uint32_t)dms);
        }
        KldDisplay::Detach(id);
    }
}

bool dispint_published(uint32_t id)
{
    SpinLockGuard g(g_disp_lock);
    DispInst* r = disp_find_locked(id);
    if (!r) return false;
    return disp_cas_locked(r, DISP_ATTACHED, DISP_PUBLISHED, "publish");
}

bool dispint_dying(uint32_t id)
{
    SpinLockGuard g(g_disp_lock);
    DispInst* r = disp_find_locked(id);
    if (!r) return false;
    if (r->state == DISP_DYING || r->state == DISP_RELEASED)
        return true;
    return disp_cas_locked(r, r->state, DISP_DYING, "dying");
}

bool dispint_release(uint32_t id)
{
    // detaching is idempotent and must happen even for a DYING record whose
    // exit cpu is between the state change and KldDisplay::Detach: otherwise
    // the bridge stays visible with g_ring/g_scanout pointing at freed frames
    // and every later attach is refused until reboot. (satoru)
    KldDisplay::DetachForRelease(id);
    DispFree d;
    {
        SpinLockGuard g(g_disp_lock);
        DispInst* r = disp_find_locked(id);
        if (!r) return false;
        if (r->state == DISP_ATTACHED)
            disp_cas_locked(r, DISP_ATTACHED, DISP_DYING, "release-detach");
        if (r->state == DISP_PUBLISHED) {
            SerialLogger::Log("[kldb] display release from PUBLISHED refused\r\n");
            return false;
        }
        if (!disp_prepare_free_locked(r, &d))
            return false;
    }
    disp_run_free(d);
    return true;
}

bool dispint_busy(uint64_t as)
{
    SpinLockGuard g(g_disp_lock);
    for (int i = 0; i < MAX_DISP_INST; i++)
        if (g_disp_inst[i].used && g_disp_inst[i].as == as &&
            g_disp_inst[i].state != DISP_RELEASED)
            return true;
    return false;
}

// vmm destroy hook: the pml4 is about to go, so the mapping ref these frames
// hold dies with it. stamp every instance of that address space under the same
// lock the final free takes; whoever runs second sees the fact and does not
// unmap or drop the ref a second time. (satoru)
void acpi_as_destroyed(uint64_t as);

void dispint_as_destroyed(uint64_t as)
{
    if (!as) return;
    acpi_as_destroyed(as);
    KldDri::AsDestroyed(as);   // drm requests of this address space are abandoned (satoru)
    // vt-d: block a dead container's devices and free its domain now, before
    // this PML4 frame can be reused: the domain is keyed by address space, so
    // a later container on the same key would otherwise inherit (or a
    // key_reused release would skip) mappings to frames the release frees.
    // idempotent: release_container's own call then finds no domain. (satoru)
    bool had_container = false;
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (g_cont[i].used && g_cont[i].as == as) had_container = true;
    if (had_container) KldIommu::ReleaseContainer(as);
    // the container record must not outlive its address space: a later process
    // can get the same PML4 frame and an ordinary munmap must not match this
    // record's old broker VAs. release_container still finds it by as. (satoru)
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (g_cont[i].used && g_cont[i].as == as) g_cont[i].as_gone = true;
    for (int i = 0; i < MAX_GRANTS; i++)
        if (g_grants[i].used && g_grants[i].as == as) g_grants[i].dead = true;
    bool* wait_free[MAX_DISP_INST];
    int n = 0;
    {
        SpinLockGuard g(g_disp_lock);
        for (int i = 0; i < MAX_DISP_INST; i++) {
            DispInst* r = &g_disp_inst[i];
            if (!r->used || r->as != as) continue;
            r->as_dead = true;   // sticky (satoru)
            if (r->freeing) wait_free[n++] = &r->freeing;
        }
    }
    if (!n) return;
    // design B: the final free unmaps and drops the frames outside
    // g_disp_lock. wait for EVERY freeing record of this address space, with
    // no path that proceeds anyway; the records stay used until their free
    // clears freeing, so the flags are stable. (satoru)
    SerialLogger::Log("[kldb] display as-destroy handshake: waiting for the final free\r\n");
    uint64_t spins = 0;
    bool warned = false;
    for (;;) {
        bool all = true;
        for (int i = 0; i < n; i++) {
            if (__atomic_load_n(wait_free[i], __ATOMIC_ACQUIRE)) {
                all = false;
                break;
            }
        }
        if (all) return;
        __asm__ __volatile__("pause");
        if (!warned && ++spins > 50000000ull) {
            warned = true;
            SerialLogger::Log("[kld-warn] display as-destroy waited for the final free\r\n");
        }
    }
}

// the frame backing container va `v` if v lies in one of c's DMA records with
// `n` pages from v also inside that same record. (satoru)
uint64_t dma_phys_of(Container* c, uint64_t v, uint32_t n)
{
    for (int j = 0; j < MAX_DMA; j++) {
        DmaRec* r = &c->dma[j];
        if (!r->used || r->freeing) continue;
        uint64_t lo = r->va;
        uint64_t hi = r->va + (uint64_t)r->pages * PAGE_SIZE;
        if (v < lo || v >= hi) continue;
        if ((uint64_t)n * PAGE_SIZE > hi - v) return 0;
        return r->phys + (v - lo);
    }
    return 0;
}

// a0=new_va, a1=src_va, a2=pages, a3=extend: map the frames behind the
// caller's own DMA range [src_va, +pages) at new_va. extend=0 starts a record
// at new_va; extend=1 appends to the caller's record that ends exactly at
// new_va. scalars only: the kernel reads no container memory. (satoru)
int64_t op_dma_alias(uint64_t as, uint64_t new_va, uint64_t src_va, uint32_t pages,
                     bool extend) {
    if (!pages || pages > MAX_ALIAS_PAGES) return KLD_EINVAL;
    if ((new_va | src_va) & 0xFFFull) return KLD_EINVAL;
    Container* c = find_container(as, false, -1);
    if (!c) return KLD_EPERM;
    uint64_t end_va = new_va + (uint64_t)pages * PAGE_SIZE;
    if (end_va < new_va) return KLD_EINVAL;
    AliasRec* a = nullptr;
    uint32_t first = 0;
    uint64_t src_phys = 0;
    {
        // the source lookup, the overlap check and the claim of the run are
        // one step under g_alias_lock; DMA_FREE takes the same lock before it
        // drops a record, so a frame cannot be freed between the lookup and
        // its first ref here. (satoru)
        SpinLockGuard g(g_alias_lock);
        src_phys = dma_phys_of(c, src_va, pages);
        if (!src_phys) return KLD_EINVAL;
        int mine = 0, freeslot = -1;
        for (int i = 0; i < MAX_ALIAS; i++) {
            AliasRec* x = &g_alias[i];
            if (!x->used) { if (freeslot < 0) freeslot = i; continue; }
            if (x->as != as) continue;
            // no run may overlap any reserved alias range of this space. (satoru)
            uint64_t xe = x->va + (uint64_t)x->reserved * PAGE_SIZE;
            if (new_va < xe && end_va > x->va) return KLD_EBUSY;
            if (x->gen != c->gen) continue;
            mine++;
            if (extend && new_va == x->va + (uint64_t)x->pages * PAGE_SIZE) a = x;
        }
        if (extend && !a) return KLD_EINVAL;
        if (a) {
            if (a->busy) return KLD_EBUSY;
            if (a->pages + pages > MAX_ALIAS_PAGES) return KLD_ENOMEM;
        } else {
            if (mine >= MAX_ALIAS_PER_CONT || freeslot < 0) return KLD_ENOSPC;
            a = &g_alias[freeslot];
            a->used = true;
            a->as = as;
            a->gen = c->gen;
            a->va = new_va;
            a->pages = 0;
            a->reserved = 0;
            a->orphan = false;
        }
        // the range is a broker range before its first pte exists. (satoru)
        a->busy = true;
        first = a->pages;
        a->reserved = first + pages;
        a->pend_phys = src_phys;
        a->pend_pages = pages;
    }
    bool vafree = va_range_free(as, new_va, pages);
    bool ok = vafree;
    uint32_t done = 0;
    if (ok) {
        uint64_t fl = PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_NX;
        for (; done < pages; done++) {
            uint64_t phys = src_phys + (uint64_t)done * PAGE_SIZE;
            PMM::RetainFrame(phys);   // mapping ref (satoru)
            if (!KernelVMM::MapPageInAddressSpace(as, new_va + (uint64_t)done * PAGE_SIZE,
                                                  phys, fl)) {
                PMM::FreeFrame(phys);
                ok = false;
                break;
            }
            PMM::RetainFrame(phys);   // alias ref (satoru)
            a->phys[first + done] = phys;
        }
    }
    if (!ok) {
        for (uint32_t j = 0; j < done; j++)
            KernelVMM::UnmapPageInAddressSpace(as, new_va + (uint64_t)j * PAGE_SIZE, false);
        if (done) broker_tlb_flush();
        for (uint32_t j = 0; j < done; j++)
            free_after_tlb_proof(a->phys[first + j], 1, 2);
    }
    {
        SpinLockGuard g(g_alias_lock);
        if (ok) {
            a->pages = first + pages;
        } else {
            a->reserved = first;
            if (first == 0) a->used = false;
        }
        a->pend_pages = 0;
        if (!a->orphan) a->busy = false;
    }
    if (a->orphan) {
        // the container was released while this run was mapped: finish that
        // release (alias refs only; the mapping refs stay with the ptes).
        // (satoru)
        for (uint32_t j = 0; j < a->pages; j++) PMM::FreeFrame(a->phys[j]);
        SpinLockGuard g(g_alias_lock);
        a->used = false;
        a->busy = false;
        a->orphan = false;
        a->pages = 0;
        a->reserved = 0;
        return KLD_EPERM;
    }
    return ok ? KLD_OK : (vafree ? KLD_ENOMEM : KLD_EBUSY);
}

// a0=va of a record start: unmap the whole alias and drop both refs of every
// frame. only the live container itself calls this. (satoru)
int64_t op_dma_unalias(uint64_t as, uint64_t va) {
    Container* c = find_container(as, false, -1);
    if (!c) return KLD_EPERM;
    AliasRec* a = nullptr;
    {
        SpinLockGuard g(g_alias_lock);
        for (int i = 0; i < MAX_ALIAS; i++) {
            AliasRec* x = &g_alias[i];
            if (x->used && x->as == as && x->gen == c->gen && x->va == va) { a = x; break; }
        }
        if (!a) return KLD_EINVAL;
        if (a->busy) return KLD_EBUSY;
        a->busy = true;
    }
    // clear every pte first, one shootdown, then the refs on proof. (satoru)
    for (uint32_t j = 0; j < a->pages; j++) {
        uint64_t v = va + (uint64_t)j * PAGE_SIZE;
        if (KernelVMM::QueryMappingInAddressSpace(as, v) == a->phys[j]) {
            KernelVMM::UnmapPageInAddressSpace(as, v, false);
        } else {
            // the range is refused to munmap, so this cannot happen; never
            // drop a mapping ref the pte does not hold. (satoru)
            SerialLogger::Log("[kld-warn] dma unalias: pte does not map the aliased frame\r\n");
            a->phys[j] |= 1;    // low bit: no mapping ref to drop (satoru)
        }
    }
    if (a->pages) broker_tlb_flush();
    for (uint32_t j = 0; j < a->pages; j++) {
        bool nomap = a->phys[j] & 1;
        free_after_tlb_proof(a->phys[j] & ~1ull, 1, nomap ? 1 : 2);   // mapping + alias ref (satoru)
    }
    {
        SpinLockGuard g(g_alias_lock);
        a->used = false;
        a->busy = false;
        a->orphan = false;   // unalias dropped every ref itself (satoru)
        a->pages = 0;
        a->reserved = 0;
    }
    return KLD_OK;
}

// true when one of c's alias records holds, or is mapping, a frame of
// [phys, +pages). the caller holds g_alias_lock. (satoru)
bool alias_holds_locked(Container* c, uint64_t phys, uint32_t pages)
{
    uint64_t end = phys + (uint64_t)pages * PAGE_SIZE;
    for (int i = 0; i < MAX_ALIAS; i++) {
        AliasRec* x = &g_alias[i];
        if (!x->used || x->as != c->as || x->gen != c->gen) continue;
        if (x->pend_pages && x->pend_phys < end &&
            x->pend_phys + (uint64_t)x->pend_pages * PAGE_SIZE > phys)
            return true;
        for (uint32_t j = 0; j < x->pages; j++)
            if (x->phys[j] >= phys && x->phys[j] < end) return true;
    }
    return false;
}

int64_t op_irq_page(uint64_t as, int pid, uint64_t va) {
    // a container the kernel did not start itself (the apitest self-test) gets
    // its record here; the family rights still come from the runner record or
    // the granted devices, never from a path. (satoru)
    Container* c = find_container(as, false, pid);
    if (!c) c = find_container(as, true, pid);
    if (!c) return KLD_EPERM;
    if (c->irq_page_phys) return KLD_EBUSY;
    if (!va_range_free(as, va, 1)) return KLD_EBUSY;
    uint64_t phys = alloc_owned(1, 52);
    if (!phys) return KLD_ENOMEM;
    if (!map_pages(as, va, phys, 1, PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_NX)) {
        free_after_tlb_proof(phys, 1, 2);
        return KLD_ENOMEM;
    }
    c->irq_page_phys = phys;
    c->irq_page_va = va;
    return KLD_OK;
}

int64_t op_msi_alloc(uint64_t as, uint32_t bdf) {
    if (g_nomsi) return KLD_ENODEV;
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
        line_reset(h);
        g_line_gsi[h] = 0xFFFFFFFFu;   // no stale line gsi on a reused slot (satoru)
        m->as = as;
        m->bdf = bdf;
        m->cap = cap;
        m->msix = false;
        m->counter_phys = c->irq_page_phys + 4ull * (uint64_t)h;
        m->counter = (volatile uint32_t*)PMM::PhysToVirt(m->counter_phys);
        m->t0slot = (volatile uint64_t*)PMM::PhysToVirt(c->irq_page_phys + KLD_IRQ_T0_OFF + 8ull * (uint64_t)h);
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
    if (m->line) {
        // mask the redirection entry, then free the vector. the 8259 stays
        // masked: the native line was free when we attached. the gsi stays in
        // g_line_gsi until after the mask so a racing isr on another cpu can
        // still mask it; line_reset then makes the slot reusable by MSI. (satoru)
        KldIoApic::Mask(m->gsi);
        __atomic_store_n(&m->used, false, __ATOMIC_RELEASE);
        m->t0slot = nullptr;   // never left pointing into a page about to be freed (satoru)
        line_reset(h);
        return;
    }
    uint32_t ctrl = cfg_read(m->bdf, m->cap + 2u, 2);
    if (m->msix) cfg_write(m->bdf, m->cap + 2u, 2, (ctrl | 0x4000u) & ~0x8000u);   // mask + disable (satoru)
    else         cfg_write(m->bdf, m->cap + 2u, 2, ctrl & ~0x1u);
    __atomic_store_n(&m->used, false, __ATOMIC_RELEASE);
    m->t0slot = nullptr;   // see the line case (satoru)
}

int64_t op_msi_free(uint64_t as, uint32_t h) {
    if (h >= MSI_VECTOR_COUNT) return KLD_EINVAL;
    SpinLockGuard g(g_kldb_lock);
    if (!g_msi[h].used || !g_msi[h].line || g_msi[h].as != as) {
        if (g_msi[h].used && g_msi[h].as == as && !g_msi[h].line) {
            msi_disable_locked((int)h);
            return KLD_OK;
        }
        return KLD_EPERM;
    }
    // a LINE handle is detached through IRQ_LINE_DETACH, not MSI_FREE: the
    // two paths differ in the 8259/ownerless-isr bookkeeping. (satoru)
    return KLD_EPERM;
}

void line_reset(int h)
{
    MsiRec* m = &g_msi[h];
    m->line = false;
    m->gsi = 0;
    m->line_flags = 0;
    m->storm_ms = 0;
    m->storm_count = 0;
    m->rate_ms = 0;
    m->rate_count = 0;
    m->rate_masked = false;
    m->storm_masked = false;
    // g_line_gsi stays: the isr fallback must be able to mask the entry after
    // a detach reset the rest of the slot (last gsi survives the reset). (satoru)
}

// ── line interrupts ─────────────────────────────────────────────────────────
// an I/O APIC redirection entry in one of the same 8 vectors MSI uses. the
// entry stays masked until the container's irq thread says it has claimed a
// delivery; the isr re-masks on every delivery so one line cannot wedge the
// vector pool. (satoru)
void line_pool_log()
{
    int nmsi = 0, nmsix = 0, nline = 0;
    for (int i = 0; i < (int)MSI_VECTOR_COUNT; i++) {
        if (!g_msi[i].used) continue;
        if (g_msi[i].line) nline++;
        else if (g_msi[i].msix) nmsix++;
        else nmsi++;
    }
    SerialLogger::Log("[kldb] vector pool ");
    SerialLogger::LogDec(nmsi + nmsix + nline);
    SerialLogger::Log("/");
    SerialLogger::LogDec((int)MSI_VECTOR_COUNT);
    SerialLogger::Log(" (msi ");
    SerialLogger::LogDec(nmsi);
    SerialLogger::Log(", msix ");
    SerialLogger::LogDec(nmsix);
    SerialLogger::Log(", line ");
    SerialLogger::LogDec(nline);
    SerialLogger::Log(")\r\n");
}

// which gsi a family may attach. pci family: the interrupt line the granted
// device's config space names (the container cannot write it). acpi family:
// the kernel cannot evaluate a GpioInt or an _PRT, so the allowed numbers are
// a list: the laptop's INTC1084 GPIO controller (gsi 14) and the LPSS i2c INTx
// (gsi 33), both level active-low, or kurono.kld.line.allow=n,n,... Test boots
// with kurono.kld.ioapic.audit=1 skip the rule (the apitest negatives attach
// arbitrary lines on purpose). (satoru)
uint32_t g_line_allow[8] = { 14, 33 };
int g_line_allow_n = 2;

bool line_gsi_allowed(Container* c, uint32_t fam, uint32_t gsi, bool isa, uint32_t flags)
{
    if (KldIoApic::AuditEnabled()) return true;
    if (fam & KldAuto::FAMILY_LINE_IRQ_ACPI) {
        if (!(flags & KldIoApic::LINE_LEVEL) || !(flags & KldIoApic::LINE_ACTIVE_LOW)) return false;
        for (int i = 0; i < g_line_allow_n; i++)
            if (g_line_allow[i] == gsi && !isa) return true;
        return false;
    }
    for (int i = 0; i < MAX_GRANTS; i++) {
        if (!g_grants[i].used || g_grants[i].as != c->as) continue;
        uint32_t il = cfg_read(g_grants[i].bdf, 0x3C, 1);
        if (isa && il == gsi && il != 0 && il != 0xFF) return true;
    }
    return false;
}

int64_t op_irq_line_attach(uint64_t as, uint32_t gsi, uint32_t flags)
{
    if (gsi >= 256) return KLD_EINVAL;
    Container* c = find_container(as, false, -1);
    if (!c || !c->irq_page_phys) return KLD_EINVAL;   // the doorbell comes first (satoru)
    uint32_t fam = family_flags_of(c);
    if (!(fam & (KldAuto::FAMILY_LINE_IRQ_ACPI | KldAuto::FAMILY_LINE_IRQ_PCI))) {
        SerialLogger::Log("[kldb] line refused: family '");
        SerialLogger::Log(c->family[0] ? c->family : "(grant-derived)");
        SerialLogger::Log("' has no line_irq manifest key\r\n");
        return KLD_EPERM;
    }
    if (!KldIoApic::Ensure()) return KLD_ENODEV;
    bool isa = (flags & KldIoApic::LINE_ISA) != 0;
    if (!line_gsi_allowed(c, fam, gsi, isa, flags)) {
        SerialLogger::Log("[kldb] line refused: gsi ");
        SerialLogger::LogDec((int)gsi);
        SerialLogger::Log(" is not one the family's device names\r\n");
        return KLD_EPERM;
    }
    uint32_t rgsi = gsi, oflags = 0;
    if (!KldIoApic::Resolve(gsi, isa, &rgsi, &oflags))
        return KLD_EINVAL;
    uint32_t eff = flags & (KldIoApic::LINE_LEVEL | KldIoApic::LINE_ACTIVE_LOW |
                            KldIoApic::LINE_SHARED);
    if (flags & KldIoApic::LINE_DEFAULT) {
        // "default": the MADT override decides for a source IRQ; without one
        // (or for a direct GSI) the bus default is edge, active-high. acpi
        // flags are polarity bits 1:0 and trigger bits 3:2, 3 = low/level.
        // (satoru)
        eff &= ~(KldIoApic::LINE_LEVEL | KldIoApic::LINE_ACTIVE_LOW);
        if (isa) {
            if ((oflags & 0x3u) == 0x3u) eff |= KldIoApic::LINE_ACTIVE_LOW;
            if (((oflags >> 2) & 0x3u) == 0x3u) eff |= KldIoApic::LINE_LEVEL;
        }
    }
    SerialLogger::Log("[kldb] line gsi ");
    SerialLogger::LogDec((int)gsi);
    SerialLogger::Log(isa ? " isa" : " gsi");
    SerialLogger::Log(" resolved ");
    SerialLogger::LogDec((int)rgsi);
    SerialLogger::Log(" caller flags 0x");
    SerialLogger::LogHex(flags);
    SerialLogger::Log(" madt 0x");
    SerialLogger::LogHex(oflags);
    SerialLogger::Log(" -> ");
    SerialLogger::Log((eff & KldIoApic::LINE_LEVEL) ? "level" : "edge");
    SerialLogger::Log((eff & KldIoApic::LINE_ACTIVE_LOW) ? " active-low" : " active-high");
    SerialLogger::Log("\r\n");
    if (KldIoApic::IsKernelOwned(rgsi)) return KLD_EPERM;
    if (KldIoApic::LegacyBusy(gsi, rgsi)) return KLD_EBUSY;
    // the destination is the bsp apic id in the low byte of the entry. an
    // x2apic id above 254 would route to the wrong cpu: refuse. (satoru)
    PerCpu* bsp0 = SMP::ByIndex(0);
    uint32_t apic_id = bsp0 ? bsp0->apic_id : 0;
    if (apic_id > 254) {
        SerialLogger::Log("[kld-ioapic] bsp apic id ");
        SerialLogger::LogDec((int)apic_id);
        SerialLogger::Log(" does not fit the redirection entry, attach refused\r\n");
        return KLD_ENODEV;
    }
    // EOI path: when firmware left EOI-broadcast suppression set in the lapic
    // SVR, a level entry's remote irr survives the local EOI and must be
    // cleared through the ioapic's direct EOI register (version >= 0x20).
    // (satoru)
    bool svr_suppress = (SMP::LapicRead(0xF0) & (1u << 12)) != 0;
    int eoi_mode = KldIoApic::EoiMode(rgsi, svr_suppress);
    uint32_t iov = KldIoApic::VersionForGsi(rgsi);
    SerialLogger::Log("[kld-ioapic] lapic svr eoi-suppress=");
    SerialLogger::LogDec(svr_suppress ? 1 : 0);
    SerialLogger::Log(", ioapic version ");
    SerialLogger::LogHex(iov);
    SerialLogger::Log(", eoi via ");
    SerialLogger::Log(eoi_mode == 1 ? "eoir" : (eoi_mode == 0 ? "broadcast" : "refused"));
    SerialLogger::Log("\r\n");
    if (eoi_mode < 0) {
        SerialLogger::Log("[kld-ioapic] EOI-broadcast suppression set and the ioapic "
                          "has no EOI register: attach refused\r\n");
        return KLD_ENODEV;
    }

    int h = -1;
    {
        SpinLockGuard g(g_kldb_lock);
        for (int i = 0; i < (int)MSI_VECTOR_COUNT; i++)
            if (g_msi[i].used && g_msi[i].line && g_msi[i].gsi == rgsi)
                return KLD_EBUSY;
        for (int i = 0; i < (int)MSI_VECTOR_COUNT; i++)
            if (!g_msi[i].used) { h = i; break; }
        if (h < 0) return KLD_ENOSPC;
        MsiRec* m = &g_msi[h];
        __atomic_store_n(&m->used, false, __ATOMIC_RELEASE);   // init under lock (satoru)
        line_reset(h);
        m->line = true;
        m->msix = false;
        m->msix_index = 0;
        m->as = as;
        m->bdf = 0;
        m->cap = 0;
        m->gsi = rgsi;
        m->line_flags = eff;
        m->eoi_suppress = (eoi_mode == 1);
        m->storm_ms = 0;
        m->storm_count = 0;
        m->rate_ms = 0;
        m->rate_count = 0;
        m->storm_masked = false;
        m->counter_phys = c->irq_page_phys + 4ull * (uint64_t)h;
        m->counter = (volatile uint32_t*)(uintptr_t)m->counter_phys;
        m->fired = 0;
        m->gen = c->gen;
        g_line_gsi[h] = rgsi;
        __atomic_store_n(&m->used, true, __ATOMIC_RELEASE);
    }
    uint32_t vec = MSI_VECTOR_BASE + (uint32_t)h;
    uint32_t apic = apic_id;
    KldIoApic::SnapshotEntries();
    if (!KldIoApic::Program(rgsi, vec, apic, eff, true)) {
        SpinLockGuard g(g_kldb_lock);
        __atomic_store_n(&g_msi[h].used, false, __ATOMIC_RELEASE);
        return KLD_EINVAL;
    }
    if (rgsi < 16) KldIoApic::MaskLegacyPic(rgsi);
    SerialLogger::Log("[kldb] line handle ");
    SerialLogger::LogDec(h);
    SerialLogger::Log(" vector 0x");
    SerialLogger::LogHex(vec);
    SerialLogger::Log(" apic ");
    SerialLogger::LogDec((int)apic);
    SerialLogger::Log("\r\n");
    line_pool_log();
    return h;
}

int64_t op_irq_line_unmask(uint64_t as, uint32_t h, uint32_t handled)
{
    if (h >= MSI_VECTOR_COUNT) return KLD_EINVAL;
    uint64_t now = Scheduler::NowMs();
    // check, guard and unmask are ONE critical section against detach/exit;
    // otherwise a detach between the check and the unmask left a live,
    // ownerless level entry firing on the bsp forever. (satoru)
    SpinLockGuard g(g_kldb_lock);
    MsiRec* m = &g_msi[h];
    if (!m->used || !m->line || m->as != as) return KLD_EPERM;
    {
        Container* cc = find_container(as, false, -1);
        if (!cc || cc->gen != m->gen) return KLD_EPERM;   // an older instance's line (satoru)
    }
    if (!handled) {
        if (now - m->storm_ms >= 1000) {
            m->storm_ms = now;
            m->storm_count = 0;
        }
        if (++m->storm_count > 64) {
            if (!m->storm_masked) {
                m->storm_masked = true;
                KldIoApic::Mask(m->gsi);
                SerialLogger::Log("[kldb] line gsi ");
                SerialLogger::LogDec((int)m->gsi);
                SerialLogger::Log(" storm: 64 unclaimed deliveries in 1s, left masked\r\n");
            }
            return KLD_EPERM;
        }
    } else {
        m->storm_ms = now;
        m->storm_count = 0;
    }
    if (m->storm_masked) return KLD_EPERM;
    // isr-side budget: 512 deliveries per 100 ms window is already far above
    // what a 400 kHz i2c touchpad can produce. over budget the entry stays
    // masked until the next window; the container cannot clear it early. (satoru)
    if (m->rate_masked) {
        if (now - m->rate_ms < 100) return KLD_EPERM;
        m->rate_masked = false;
        m->rate_ms = now;
        m->rate_count = 0;
    }
    // both level and edge entries are unmasked here: an edge line is never
    // masked on delivery, but it starts masked and needs this first unmask. (satoru)
    return KldIoApic::Unmask(m->gsi) ? KLD_OK : KLD_EINVAL;
}

int64_t op_irq_line_detach(uint64_t as, uint32_t h)
{
    if (h >= MSI_VECTOR_COUNT) return KLD_EINVAL;
    SpinLockGuard g(g_kldb_lock);
    if (!g_msi[h].used || !g_msi[h].line || g_msi[h].as != as) return KLD_EPERM;
    {
        Container* cc = find_container(as, false, -1);
        if (!cc || cc->gen != g_msi[h].gen) return KLD_EPERM;   // an older instance's line (satoru)
    }
    uint32_t detached_gsi = g_msi[h].gsi;
    msi_disable_locked((int)h);
    if (detached_gsi)
        KldIoApic::CheckEntriesExcept(detached_gsi);
    SerialLogger::Log("[kldb] line handle ");
    SerialLogger::LogDec((int)h);
    SerialLogger::Log(" detached\r\n");
    line_pool_log();
    return KLD_OK;
}

// ── platform mmio safety ────────────────────────────────────────────────────
// a platform range must not be a pci bar, the ecam window, the iommu's drhd
// register page, the hpet block, or a range another container already mapped.
// the tables are parsed read-only through the boot rsdp; the broker never
// touches the hardware they describe. (satoru)
struct AcpiSdtHdr {
    uint8_t  sig[4];
    uint32_t length;
    uint8_t  revision, checksum;
    uint8_t  oem_id[6];
    uint8_t  oem_table[8];
    uint32_t oem_rev, creator, creator_rev;
} __attribute__((packed));

const AcpiSdtHdr* acpi_sdt(const char sig[4])
{
    uint64_t rsdp = BootAcpi::Rsdp();
    // high-half kernel static; the firmware table pointers inside are physical
    // and are bounded below. (satoru)
    if (!rsdp) return nullptr;
    const uint8_t* r = (const uint8_t*)(uintptr_t)rsdp;
    const AcpiSdtHdr* root = nullptr;
    if (r[15] >= 2) {
        uint64_t xsdt = *(const uint64_t*)(r + 24);
        const AcpiSdtHdr* t = (const AcpiSdtHdr*)(uintptr_t)xsdt;
        if (xsdt && xsdt <= PMM::IDENTITY_CEIL_BYTES && xsdt + sizeof(AcpiSdtHdr) <= PMM::IDENTITY_CEIL_BYTES &&
            t->length >= sizeof(AcpiSdtHdr) && t->length <= (1u << 20) &&
            t->sig[0] == 'X' && t->sig[1] == 'S' && t->sig[2] == 'D' && t->sig[3] == 'T')
            root = t;
    }
    if (!root) {
        uint32_t rsdt = *(const uint32_t*)(r + 16);
        const AcpiSdtHdr* t = (const AcpiSdtHdr*)(uintptr_t)rsdt;
        if (rsdt && rsdt + sizeof(AcpiSdtHdr) <= PMM::IDENTITY_CEIL_BYTES &&
            t->length >= sizeof(AcpiSdtHdr) && t->length <= (1u << 20) &&
            t->sig[0] == 'R' && t->sig[1] == 'S' && t->sig[2] == 'D' && t->sig[3] == 'T')
            root = t;
    }
    if (!root) return nullptr;
    bool wide = root->sig[0] == 'X';
    uint32_t esz = wide ? 8 : 4;
    uint32_t n = (root->length - sizeof(AcpiSdtHdr)) / esz;
    const uint8_t* p = (const uint8_t*)root + sizeof(AcpiSdtHdr);
    for (uint32_t i = 0; i < n; i++) {
        uint64_t tp = wide ? *(const uint64_t*)(p + i * 8)
                           : *(const uint32_t*)(p + i * 4);
        const AcpiSdtHdr* t = (const AcpiSdtHdr*)(uintptr_t)tp;
        if (!tp || tp > PMM::IDENTITY_CEIL_BYTES || tp + sizeof(AcpiSdtHdr) > PMM::IDENTITY_CEIL_BYTES) continue;   // no wrap on a corrupt entry (satoru)
        if (t->length < sizeof(AcpiSdtHdr) || t->length > (1u << 20) ||
            tp + t->length > PMM::IDENTITY_CEIL_BYTES) continue;
        if (t->sig[0] == (uint8_t)sig[0] && t->sig[1] == (uint8_t)sig[1] &&
            t->sig[2] == (uint8_t)sig[2] && t->sig[3] == (uint8_t)sig[3])
            return t;
    }
    return nullptr;
}

bool ov(uint64_t a, uint64_t alen, uint64_t b, uint64_t blen)
{
    return alen && blen && a < b + blen && b < a + alen;
}

bool overlaps_pci_bar(uint64_t phys, uint64_t size)
{
    scan_bus_once();   // cached; never sizes a device after boot (satoru)
    if (g_scan_truncated)
        return true;   // incomplete list: refuse everything (satoru)
    for (int i = 0; i < g_dev_count; i++) {
        for (uint32_t idx = 0; idx < g_devs[i].bar_count; idx++) {
            uint64_t base = g_devs[i].bar_base[idx];
            uint64_t len = g_devs[i].bar_size[idx];
            if (base && len && ov(phys, size, base, len))
                return true;
        }
    }
    return false;
}

// the refusal data is parsed ONCE at broker init into kernel memory. a
// container that could rewrite the live DMAR/MCFG/HPET table (they live in
// acpi reclaim/nvs) must not be able to change what the kernel refuses. (satoru)
struct MmioRange { uint64_t lo, hi; };
MmioRange g_ecam[16];  int g_necam;
MmioRange g_drhd[16];  int g_ndrhd;
MmioRange g_hpet[4];   int g_nhpet;
// the TPM's locality registers are never a container mmio range. (satoru)
constexpr uint64_t TPM_LO = 0xFED40000ULL, TPM_HI = 0xFED45000ULL;

void push_range(MmioRange* arr, int* n, int mx, uint64_t lo, uint64_t hi)
{
    if (*n < mx && hi > lo) {
        arr[*n].lo = lo;
        arr[*n].hi = hi;
        (*n)++;
    }
}

// FADT ports whose WRITES are never the container's, in every mode: the SMI
// command port and the PM1 control blocks (sleep types, SCI_EN, power off).
// read once at init from the tables, like the ranges above. (satoru)
uint32_t g_fadt_smi_cmd, g_fadt_pm1a_cnt, g_fadt_pm1b_cnt;
uint8_t  g_fadt_pm1_cnt_len;

void build_refusal_ranges()
{
    g_necam = g_ndrhd = g_nhpet = 0;
    if (const AcpiSdtHdr* f = acpi_sdt("FACP")) {
        if (f->length >= 90) {
            const uint8_t* b = (const uint8_t*)f;
            g_fadt_smi_cmd = *(const uint32_t*)(b + 48);
            g_fadt_pm1a_cnt = *(const uint32_t*)(b + 64);
            g_fadt_pm1b_cnt = *(const uint32_t*)(b + 68);
            g_fadt_pm1_cnt_len = b[89];
        }
    }
    const AcpiSdtHdr* t = acpi_sdt("MCFG");
    if (t) {
        const uint8_t* p = (const uint8_t*)t + sizeof(AcpiSdtHdr) + 8;
        const uint8_t* end = (const uint8_t*)t + t->length;
        while (p + 16 <= end) {
            uint64_t base = *(const uint64_t*)p;
            uint8_t start = p[10], endbus = p[11];
            uint64_t len = ((uint64_t)(endbus - start) + 1) << 20;
            push_range(g_ecam, &g_necam, 16, base, base + len);
            p += 16;
        }
    }
    t = acpi_sdt("DMAR");
    if (t) {
        // INTEGRATION HOOK: when kld_iommu's dmar table parser is merged, use
        // its drhd list (and the real register-block size) here. (satoru)
        const uint8_t* p = (const uint8_t*)t + sizeof(AcpiSdtHdr) + 12;
        const uint8_t* end = (const uint8_t*)t + t->length;
        while (p + 4 <= end) {
            uint16_t type = *(const uint16_t*)p;
            uint16_t len = *(const uint16_t*)(p + 2);
            if (len < 4 || p + len > end) break;
            if (type == 0 && len >= 16) {
                uint64_t base = *(const uint64_t*)(p + 8) & ~0xFFFULL;
                // the DRHD size byte N means 2^(N+12) bytes (linux dmar.c);
                // clamping keeps a bad firmware value from wrapping. (satoru)
                uint8_t n = p[5];
                uint64_t sz = (n <= 20) ? (1ull << (n + 12)) : 0x1000ull;
                push_range(g_drhd, &g_ndrhd, 16, base, base + sz);
            }
            p += len;
        }
    }
    t = acpi_sdt("HPET");
    if (t && t->length >= 56) {
        uint64_t base = *(const uint64_t*)((const uint8_t*)t + 44) & ~0xFFFULL;
        push_range(g_hpet, &g_nhpet, 4, base, base + 0x1000);
    }
    SerialLogger::Log("[kldb] refusal ranges cached: ecam ");
    SerialLogger::LogDec(g_necam);
    SerialLogger::Log(", drhd ");
    SerialLogger::LogDec(g_ndrhd);
    SerialLogger::Log(", hpet ");
    SerialLogger::LogDec(g_nhpet);
    SerialLogger::Log("\r\n");
}

bool overlaps_cached(MmioRange* arr, int n, uint64_t phys, uint64_t size)
{
    for (int i = 0; i < n; i++)
        if (ov(phys, size, arr[i].lo, arr[i].hi - arr[i].lo)) return true;
    return false;
}

bool overlaps_ecam(uint64_t phys, uint64_t size) { return overlaps_cached(g_ecam, g_necam, phys, size); }
bool overlaps_drhd(uint64_t phys, uint64_t size) { return overlaps_cached(g_drhd, g_ndrhd, phys, size); }
bool overlaps_hpet(uint64_t phys, uint64_t size) { return overlaps_cached(g_hpet, g_nhpet, phys, size); }

bool mapped_elsewhere(uint64_t as, uint64_t phys, uint64_t size)
{
    for (int ci = 0; ci < MAX_CONTAINERS; ci++) {
        Container* c = &g_cont[ci];
        if (!c->used || c->as == as || c->as_gone) continue;   // as_gone: like RangeIsBrokerMapping (satoru)
        for (int m = 0; m < MAX_MAPS; m++) {
            if (!c->maps[m].used || !c->maps[m].phys) continue;
            if (ov(phys, size, c->maps[m].phys,
                   (uint64_t)c->maps[m].pages * PAGE_SIZE))
                return true;
        }
    }
    return false;
}

// ── acpi write allowlist (kurono.kld.acpi.allow=...) ────────────────────────
// the kernel starts with NO writable acpi register and no writable opregion.
// the owner adds entries at boot; everything else is refused at the hardware
// and shadowed for acpica's read-back. syntax, comma separated, max 8:
//   port:0x70:8               one port write
//   pci:0:2.0:0x10:4          one pci config dword
//   mem:0x7fe00000:0x1000     one writable operation-region range
// (satoru)
constexpr int MAX_ACPI_ALLOW = 8;
struct AcpiAllow { uint8_t kind; uint32_t a, b, c; };   // 0 port, 1 pci, 2 mem
AcpiAllow g_acpi_allow[MAX_ACPI_ALLOW];
int g_acpi_allow_n;


bool allow_port(uint32_t port, uint32_t width)
{
    for (int i = 0; i < g_acpi_allow_n; i++)
        if (g_acpi_allow[i].kind == 0 && g_acpi_allow[i].a == port &&
            g_acpi_allow[i].b == width)
            return true;
    return false;
}

bool allow_pci(uint32_t bdf, uint32_t off, uint32_t width)
{
    for (int i = 0; i < g_acpi_allow_n; i++)
        if (g_acpi_allow[i].kind == 1 && g_acpi_allow[i].a == bdf &&
            g_acpi_allow[i].b == off && g_acpi_allow[i].c == width)
            return true;
    return false;
}

bool allow_mem(uint64_t phys, uint64_t len)
{
    for (int i = 0; i < g_acpi_allow_n; i++)
        if (g_acpi_allow[i].kind == 2 && phys >= g_acpi_allow[i].a &&
            phys + len <= (uint64_t)g_acpi_allow[i].a + g_acpi_allow[i].b)
            return true;
    return false;
}

// log-once tables for the allowed acpi reads, bounded so a container cannot
// flood the serial log with one line per port. (satoru)
uint32_t g_port_read_logged[8];
int g_port_read_logged_n;
uint32_t g_pci_read_logged[8];
int g_pci_read_logged_n;

void log_port_read_once(uint32_t port)
{
    for (int i = 0; i < g_port_read_logged_n; i++)
        if (g_port_read_logged[i] == port) return;
    if (g_port_read_logged_n < 8) {
        g_port_read_logged[g_port_read_logged_n++] = port;
        SerialLogger::Log("[kldb] acpi port read 0x");
        SerialLogger::LogHex(port);
        SerialLogger::Log(" (first use)\r\n");
    }
}

void log_pci_read_once(uint32_t bdf)
{
    for (int i = 0; i < g_pci_read_logged_n; i++)
        if (g_pci_read_logged[i] == bdf) return;
    if (g_pci_read_logged_n < 8) {
        g_pci_read_logged[g_pci_read_logged_n++] = bdf;
        log_bdf("[kldb] acpi pci read of ", bdf);
        SerialLogger::Log(" (first use)\r\n");
    }
}

// ── platform mmio ───────────────────────────────────────────────────────────
// the pch gpio communities are not pci bars: acpi _crs names them and the
// touchpad family maps them here. a pci bar must keep using MAP_BAR (grant +
// msi-x table rules); this op is only for the platform ranges the manifest
// declares. (satoru)
// the exact windows a platform_mmio family may map. the kernel has no AML
// interpreter, so it cannot read the GPIO community _CRS itself; the manifest
// rule is therefore the SHAPE of the intel pch sideband communities: one 64 KiB
// window per community, 64 KiB aligned, at the community port offsets 0x69,
// 0x6A, 0x6B, 0x6D, 0x6E inside the 16 MiB-aligned P2SB sideband base (the
// laptop's INTC1084 communities). the 16 MiB window as a whole, any other
// port and any other size are never mappable. (satoru)
bool plat_window_ok(uint64_t phys, uint64_t size)
{
    if (size != 0x10000ull || (phys & 0xFFFFull)) return false;
    uint64_t off = phys & 0xFFFFFFull;
    return off == 0x690000 || off == 0x6A0000 || off == 0x6B0000 ||
           off == 0x6D0000 || off == 0x6E0000;
}

int64_t op_plat_map(uint64_t as, uint64_t phys, uint64_t bytes, uint64_t va)
{
    Container* c = find_container(as, false, -1);
    if (!c || !c->family[0]) return KLD_EPERM;
    uint32_t fam = family_flags_of(c);
    if (!(fam & KldAuto::FAMILY_PLATFORM_MMIO_ACPI)) {
        SerialLogger::Log("[kldb] plat map refused: family '");
        SerialLogger::Log(c->family[0] ? c->family : "(grant-derived)");
        SerialLogger::Log("' has no platform_mmio manifest key\r\n");
        return KLD_EPERM;
    }
    if (!phys || (phys & 0xFFF) || !bytes || bytes > (4ull << 20))
        return KLD_EINVAL;
    uint64_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t size = pages * PAGE_SIZE;
    if (phys + size < phys || phys + size > (1ull << 52))
        return KLD_EINVAL;
    if (!plat_window_ok(phys, size)) {
        SerialLogger::Log("[kldb] plat map refused: not one of the family's exact windows 0x");
        SerialLogger::LogHex64(phys);
        SerialLogger::Log(" len 0x");
        SerialLogger::LogHex64(size);
        SerialLogger::Log("\r\n");
        return KLD_EPERM;
    }
    if (c->plat_pages + pages > 1024) {   // 4 MiB per container, not per call (satoru)
        SerialLogger::Log("[kldb] plat map refused: per-container 4 MiB budget\r\n");
        return KLD_ENOMEM;
    }
    if (c->plat_maps >= 32) {
        SerialLogger::Log("[kldb] plat map refused: 32 maps per container\r\n");
        return KLD_ENOSPC;
    }
    if (phys < 0x100000) {   // bios/ivt/low memory is never device mmio (satoru)
        SerialLogger::Log("[kldb] plat map refused: low memory 0x");
        SerialLogger::LogHex64(phys);
        SerialLogger::Log("\r\n");
        return KLD_EPERM;
    }
    if (PhysIsUsableRam(phys, size)) {
        SerialLogger::Log("[kldb] plat map refused: usable ram 0x");
        SerialLogger::LogHex64(phys);
        SerialLogger::Log("\r\n");
        return KLD_EPERM;
    }
    // PLAT_MAP is for device mmio holes: never ACPI reclaim (3) or NVS (4).
    // type 2 is the firmware category MMIO holes themselves live in, so the
    // specific dangerous reserved ranges are the cached refusal lists below.
    // (satoru)
    uint32_t etype = map_type_any(phys, size);
    // type 2 (reserved) is allowed here: firmware may list the sideband window
    // that way, and only the exact windows above get this far. (satoru)
    if (etype != 0 && etype != 2) {
        // every E820-described range (usable, reserved, acpi reclaim, nvs,
        // bad) is refused; only true holes are device mmio. (satoru)
        SerialLogger::Log("[kldb] plat map refused: e820 type ");
        SerialLogger::LogDec((int)etype);
        SerialLogger::Log(" 0x");
        SerialLogger::LogHex64(phys);
        SerialLogger::Log("\r\n");
        return KLD_EPERM;
    }
    if ((phys < 0xFEE01000ULL && phys + size > 0xFEE00000ULL) ||
        KldIoApic::OwnsRange(phys, size)) {
        SerialLogger::Log("[kldb] plat map refused: interrupt hardware 0x");
        SerialLogger::LogHex64(phys);
        SerialLogger::Log("\r\n");
        return KLD_EPERM;
    }
    if (overlaps_pci_bar(phys, size)) {
        SerialLogger::Log("[kldb] plat map refused: pci bar 0x");
        SerialLogger::LogHex64(phys);
        SerialLogger::Log(" (a bar uses MAP_BAR)\r\n");
        return KLD_EPERM;
    }
    if (ov(phys, size, TPM_LO, TPM_HI - TPM_LO)) {
        SerialLogger::Log("[kldb] plat map refused: tpm block 0x");
        SerialLogger::LogHex64(phys);
        SerialLogger::Log("\r\n");
        return KLD_EPERM;
    }
    if (overlaps_ecam(phys, size)) {
        SerialLogger::Log("[kldb] plat map refused: mcfg ecam window 0x");
        SerialLogger::LogHex64(phys);
        SerialLogger::Log("\r\n");
        return KLD_EPERM;
    }
    if (overlaps_drhd(phys, size)) {
        SerialLogger::Log("[kldb] plat map refused: dmar drhd page 0x");
        SerialLogger::LogHex64(phys);
        SerialLogger::Log("\r\n");
        return KLD_EPERM;
    }
    if (overlaps_hpet(phys, size)) {
        SerialLogger::Log("[kldb] plat map refused: hpet block 0x");
        SerialLogger::LogHex64(phys);
        SerialLogger::Log("\r\n");
        return KLD_EPERM;
    }
    if (mapped_elsewhere(as, phys, size)) {
        SerialLogger::Log("[kldb] plat map refused: range already mapped by another container 0x");
        SerialLogger::LogHex64(phys);
        SerialLogger::Log("\r\n");
        return KLD_EBUSY;
    }
    if (!va_range_free(as, va, pages)) return KLD_EBUSY;
    MapRec* m = nullptr;
    for (int i = 0; i < MAX_MAPS; i++) if (!c->maps[i].used) { m = &c->maps[i]; break; }
    if (!m) return KLD_ENOSPC;
    uint64_t fl = PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_PCD | PTE_PWT | PTE_NX;
    if (!map_pages(as, va, phys, pages, fl)) return KLD_ENOMEM;
    // retain twice per page like MAP_BAR: free_user_tree frees every present
    // user leaf, and a gpio page must never land on the PMM free list. the refs
    // stay for the life of the machine (device memory is not kernel memory).
    // (satoru)
    for (uint64_t i = 0; i < pages; i++) {
        PMM::RetainFrame(phys + i * PAGE_SIZE);
        PMM::RetainFrame(phys + i * PAGE_SIZE);
    }
    m->used = true;
    m->plat = true;
    m->va = va;
    m->phys = phys;
    m->pages = (uint32_t)pages;
    c->plat_pages += (uint32_t)pages;
    c->plat_maps++;
    SerialLogger::Log("[kldb] plat map phys 0x");
    SerialLogger::LogHex64(phys);
    SerialLogger::Log(" pages ");
    SerialLogger::LogDec((int)pages);
    SerialLogger::Log(" -> va 0x");
    SerialLogger::LogHex64(va);
    SerialLogger::Log(" (uc,nx) family ");
    SerialLogger::Log(c->family);
    SerialLogger::Log("\r\n");
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

// 0 = not an msi-x page, 1 = table page, 2 = pba page. (satoru)
int msix_page_kind(uint32_t bdf, uint64_t page) {
    MsixInfo mi;
    if (!msix_info(bdf, &mi)) return 0;
    uint64_t t0 = mi.table & ~0xFFFull, t1 = (mi.table + mi.size * 16ull + 0xFFFull) & ~0xFFFull;
    uint64_t p0 = mi.pba & ~0xFFFull,   p1 = (mi.pba + mi.pba_bytes + 0xFFFull) & ~0xFFFull;
    if (page >= p0 && page < p1) return 2;
    if (page >= t0 && page < t1) return 1;
    return 0;
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
    if (g_nomsi) return KLD_ENODEV;
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
            line_reset((int)(first + i));
            g_line_gsi[first + i] = 0xFFFFFFFFu;   // no stale line gsi on a reused slot (satoru)
            m->as = as; m->bdf = bdf; m->cap = mi.cap;
            m->msix = true; m->msix_index = i;
            m->counter_phys = c->irq_page_phys + 4ull * (uint64_t)(first + i);
            m->counter = (volatile uint32_t*)PMM::PhysToVirt(m->counter_phys);
            m->t0slot = (volatile uint64_t*)PMM::PhysToVirt(c->irq_page_phys + KLD_IRQ_T0_OFF + 8ull * (uint64_t)(first + i));
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
    SpinLockGuard g(g_kldb_lock);
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
        if (grant_live(g_grants[i], as) && g_grants[i].bdf == bdf) return g_msix_granted[i];
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
        free_after_tlb_proof(phys, pages, 2);
        return KLD_ENOMEM;
    }
    uint8_t mac[6];
    for (int i = 0; i < 4; i++) mac[i] = (uint8_t)(mac_lo >> (8 * i));
    mac[4] = (uint8_t)mac_hi; mac[5] = (uint8_t)(mac_hi >> 8);
    int64_t r = KldNet::Attach(as, phys, mac);
    if (r < 0) {
        unmap_pages(as, va, pages);
        free_after_tlb_proof(phys, pages, 2);
        return r;
    }
    c->net_phys = phys;
    c->net_va = va;
    return KLD_OK;
}

void release_container(Container* c) {
    // a dead record whose PML4 frame already carries a new live container:
    // teardown keyed only by address space (iommu domain, net backend) belongs
    // to the live one now; bus mastering is off for the dead grants below, so
    // their devices cannot dma. (satoru)
    const bool key_reused = c->as_gone && find_container(c->as, false, -1) != nullptr;
    if (key_reused)
        SerialLogger::Log("[kld-warn] release of a dead container whose address space was reused: iommu/net teardown left to the live one\r\n");
    if (!key_reused) KldDri::ContainerGone(c->as);   // open /dev/dri files turn dead; a live container on the reused key keeps its own (satoru)
    // 1) bus mastering off on every granted device: the device must stop
    //    writing into frames before any of them can be freed. (satoru)
    for (int i = 0; i < MAX_GRANTS; i++) {
        if (!grant_of(g_grants[i], c->as, key_reused)) continue;
        uint32_t bdf = g_grants[i].bdf;
        uint32_t cmd = cfg_read(bdf, 0x04, 2);
        cfg_write(bdf, 0x04, 2, cmd & ~0x4u);
    }
    // 2) msi off + vectors free. the lock pairs with UNMASK: a sibling that
    //    passed its check cannot unmask a slot this release just reset.
    //    only vectors of this record's own granted devices. (satoru)
    {
        SpinLockGuard g(g_kldb_lock);
        for (int h = 0; h < (int)MSI_VECTOR_COUNT; h++) {
            if (!g_msi[h].used || g_msi[h].as != c->as) continue;
            bool mine = false;
            if (g_msi[h].line) {
                // a line record has no bdf: it is this record's when it was
                // attached by this container instance (generation), even when
                // a live container has since taken the same address space key.
                // (satoru)
                mine = (g_msi[h].gen == c->gen);
            } else {
                for (int i = 0; i < MAX_GRANTS && !mine; i++)
                    mine = grant_of(g_grants[i], c->as, key_reused) && g_grants[i].bdf == g_msi[h].bdf;
            }
            if (mine || !key_reused) msi_disable_locked(h);
        }
    }
    // 2b) a delivery already past the used-check on another cpu may still be
    // writing the doorbell counter. masking/disabling stops NEW deliveries;
    // this wait covers the one in flight before step 4 frees the page. the isr
    // path is a handful of stores, so a short bounded spin is sufficient. a
    // full barrier orders the used=false stores above against the counter
    // loads below (the plain load could otherwise be hoisted). (satoru)
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    for (int h = 0; h < (int)MSI_VECTOR_COUNT; h++) {
        for (uint64_t spins = 0; __atomic_load_n(&g_vec_in_flight[h], __ATOMIC_ACQUIRE); spins++) {
            if ((spins & 0xFFFFF) == 0xFFFFF) {
                SerialLogger::Log("[kldb] WARNING: vector ");
                SerialLogger::LogDec(h);
                SerialLogger::Log(" still in flight at release\r\n");
            }
            __asm__ __volatile__("pause");
        }
    }
    // 2a) vmap aliases of this container's frames: drop the alias ref only.
    // the mapping ref belongs to the pte and dies with the address space
    // teardown (or stays valid while a live space still maps it), exactly
    // like a DMA record's. (satoru)
    // a record another cpu is still inside (alias/unalias) is marked orphan
    // under the lock and that op finishes the release; otherwise this claims
    // it (busy) and frees outside the lock. (satoru)
    for (int i = 0; i < MAX_ALIAS; i++) {
        AliasRec* a = &g_alias[i];
        {
            SpinLockGuard g(g_alias_lock);
            if (!a->used || a->as != c->as || a->gen != c->gen) continue;
            if (a->busy) { a->orphan = true; continue; }
            a->busy = true;
        }
        for (uint32_t j = 0; j < a->pages; j++) PMM::FreeFrame(a->phys[j]);
        SpinLockGuard g(g_alias_lock);
        a->used = false;
        a->busy = false;
        a->orphan = false;
        a->pages = 0;
        a->reserved = 0;
    }
    // 2b) vt-d: block the device and free its domain tables before any frame
    // it could still write is returned. bus mastering and msi are already
    // off above. (satoru)
    if (!key_reused) KldIommu::ReleaseContainer(c->as);
    // 3) the display bridge stops reading the ring and the scanout. the
    // address-space teardown already dropped the mapping ref; the gui frees
    // the broker ref through the instance id. (satoru)
    if (c->disp_id) {
        // the container is gone; the address space is dead or about to be
        // (the vmm hook stamps the instance if it dies later). pass the
        // instance id, never the address space. (satoru)
        uint32_t id = c->disp_id;
        c->disp_id = 0;
        dispint_exit(id, true);
    }
    // 4) the net backend stops reading the rings. (satoru)
    if (c->net_phys) {
        if (!key_reused) KldNet::Detach(c->as);
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
    // 5) acpi provider accounting: how much actually reached the hardware.
    if (c->acpi_touched) {
        SerialLogger::Log("[kldb] acpi io for this container: real port writes ");
        SerialLogger::LogDec((int)c->acpi_real_port_w);
        SerialLogger::Log(", real pci writes ");
        SerialLogger::LogDec((int)c->acpi_real_pci_w);
        SerialLogger::Log(", shadowed port ");
        SerialLogger::LogDec((int)c->acpi_shadow_port_w);
        SerialLogger::Log(", pci ");
        SerialLogger::LogDec((int)c->acpi_shadow_pci_w);
        SerialLogger::Log("\r\n");
    }
    // 6) grants. (satoru)
    for (int i = 0; i < MAX_GRANTS; i++) {
        if (grant_of(g_grants[i], c->as, key_reused)) {
            log_bdf("[kldb] released ", g_grants[i].bdf);
            SerialLogger::Log(" (bus master off, msi off, frames returned)\r\n");
            g_grants[i].used = false;
        }
    }
    c->used = false;
    // the runner's context may wait for acks: buy the proof for anything
    // queued (this container's last unmaps included). (satoru)
    tlbq_drain(true);
}

// ── ACPI container support ──────────────────────────────────────────────────
struct MapEnt { uint64_t base, len; uint32_t type; };
constexpr int MAP_MAX = 256;   // QEMU shows about 10 entries, a laptop 40 to 100 (satoru)
MapEnt g_map[MAP_MAX];
int g_map_n = 0;
bool g_map_trunc = false;      // more firmware entries than MAP_MAX: fail closed (satoru)
uint64_t g_acpi_as = 0;
uint32_t g_acpi_refused = 0;

// the kernel's own acpi runner, plus any driver family whose manifest asks for
// acpi services (the touchpad family needs the namespace, _PRT and _CRS).
// (satoru)
bool is_acpi(uint64_t as)
{
    if (g_acpi_as && as == g_acpi_as)
        return true;
    Container* c = find_container(as, false, -1);
    if (!c)
        return false;
    uint32_t f = family_flags_of(c);
    return (f & (KldAuto::FAMILY_LINE_IRQ_ACPI |
                 KldAuto::FAMILY_PLATFORM_MMIO_ACPI)) != 0;
}

// a driver family (not the kernel's own kld-acpi runner) runs under the strict
// acpi policy: opregion maps are read-only unless allowlisted, and port/pci
// writes are refused at the hardware unless allowlisted. (satoru)
bool acpi_strict(uint64_t as)
{
    if (g_acpi_as && as == g_acpi_as)
        return false;
    Container* c = find_container(as, false, -1);
    if (!c)
        return false;
    uint32_t f = family_flags_of(c);
    return (f & (KldAuto::FAMILY_LINE_IRQ_ACPI |
                 KldAuto::FAMILY_PLATFORM_MMIO_ACPI)) != 0;
}

Container* acpi_container(uint64_t as)
{
    // the kld-acpi runner has no SetDmaBudgetForName record; create one so the
    // per-container shadow and counters exist. the identity is the broker's
    // (it is only called after is_acpi). (satoru)
    Container* c = find_container(as, false, -1);
    if (!c) c = find_container(as, true, -1);
    if (c) c->acpi_touched = true;
    return c;
}

// the family is whatever the KERNEL recorded when it started the container
// (SetDmaBudgetForName in the runner). a container the kernel did not start
// itself (the net bench execs the driver) has no record; derive the rights
// from the DEVICES the broker granted it, matched through the kernel's own
// table. no path string the container can choose is ever consulted. (satoru)
uint32_t family_flags_of(Container* c)
{
    if (!c) return 0;
    if (c->family[0])
        return KldAuto::FamilyFlags(c->family);
    uint32_t acc = 0;
    for (int i = 0; i < MAX_GRANTS; i++) {
        if (!g_grants[i].used || g_grants[i].as != c->as) continue;
        for (int d = 0; d < g_dev_count; d++) {
            if (g_devs[d].bdf != g_grants[i].bdf) continue;
            const char* fam = nullptr;
            bool need = false;
            if (KldAuto::MatchDevice(g_devs[d].vendor, g_devs[d].device,
                                     g_devs[d].subvendor, g_devs[d].subdevice,
                                     g_devs[d].class_code, &fam, &need) && fam)
                acc |= KldAuto::FamilyFlags(fam);
            break;
        }
    }
    return acc;
}

// ── acpi write shadow ───────────────────────────────────────────────────────
// acpica writes a few registers the kernel owns (the pic/pit/8042/cmos ports,
// a pci bar or msi body) and then reads them back to verify. the write is
// REFUSED at the hardware and remembered here, so the container's read sees
// what it wrote while the kernel's state is untouched. (satoru)
constexpr int MAX_ACPI_SHADOW = 16;

// per container: one container's refused writes never leak into another's
// read-back, and a full table refuses the write (with a log) instead of
// silently dropping it. (satoru)
bool shadow_put(uint64_t as, bool pci, uint32_t addr, uint32_t width, uint32_t value)
{
    Container* c = find_container(as, false, -1);
    if (!c) c = find_container(as, true, -1);
    if (!c) return false;
    SpinLockGuard g(g_kldb_lock);
    for (int i = 0; i < MAX_ACPI_SHADOW; i++) {
        if (c->acpi_shadow[i].used && c->acpi_shadow[i].pci == pci &&
            c->acpi_shadow[i].addr == addr && c->acpi_shadow[i].width == width) {
            c->acpi_shadow[i].value = value;
            return true;
        }
    }
    for (int i = 0; i < MAX_ACPI_SHADOW; i++) {
        if (!c->acpi_shadow[i].used) {
            c->acpi_shadow[i].used = true;
            c->acpi_shadow[i].pci = pci;
            c->acpi_shadow[i].addr = addr;
            c->acpi_shadow[i].width = width;
            c->acpi_shadow[i].value = value;
            return true;
        }
    }
    return false;
}

bool shadow_get(uint64_t as, bool pci, uint32_t addr, uint32_t width, uint32_t* out)
{
    Container* c = find_container(as, false, -1);
    if (!c) return false;
    SpinLockGuard g(g_kldb_lock);
    for (int i = 0; i < MAX_ACPI_SHADOW; i++) {
        if (c->acpi_shadow[i].used && c->acpi_shadow[i].pci == pci &&
            c->acpi_shadow[i].addr == addr && c->acpi_shadow[i].width == width) {
            *out = c->acpi_shadow[i].value;
            return true;
        }
    }
    return false;
}

// the acpi privilege is keyed by address space as well: it dies with it.
// (satoru)
void acpi_as_destroyed(uint64_t as) {
    if (g_acpi_as && g_acpi_as == as) {
        g_acpi_as = 0;
        SerialLogger::Log("[kldb] acpi privilege revoked (address space destroyed)\r\n");
    }
}

// type of the firmware map entry covering phys, or 0 if none. (satoru)
uint32_t map_type(uint64_t phys) {
    if (g_map_trunc) return 1;   // incomplete map: everything looks like ram (satoru)
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

// first DESCRIBED firmware type in the range (0 = every page is a true hole).
// PLAT_MAP is for device mmio holes only: one described page makes the whole
// range non-mappable, whatever the first page said. (satoru)
uint32_t map_type_any(uint64_t phys, uint64_t len)
{
    uint32_t worst = 0;
    for (uint64_t p = phys; p < phys + len; p += PAGE_SIZE) {
        uint32_t t = map_type(p);
        if (!t) continue;
        // keep the strictest class seen: usable > acpi reclaim/nvs > reserved/bad
        if (worst == 0 || t == 1) worst = t;
        else if (worst != 1 && t == 3) worst = 3;
        else if (worst != 1 && worst != 3 && t == 4) worst = 4;
    }
    return worst;
}

int64_t op_acpi_map_phys(uint64_t as, uint64_t phys, uint64_t bytes, uint64_t va) {
    if (!g_map_n || g_map_trunc) return KLD_EPERM;   // no/partial firmware map: refuse everything (satoru)
    acpi_container(as);
    uint64_t base = phys & ~0xFFFull;
    uint64_t pages = ((phys + bytes + 0xFFF) & ~0xFFFull) - base;
    pages /= PAGE_SIZE;
    if (!pages || pages > 4096) return KLD_EINVAL;
    uint64_t total = pages * PAGE_SIZE;
    if (base + total < base || base + total > (1ull << 52))
        return KLD_EINVAL;
    if (base < 0x100000 || (base < 0xFEE01000ULL && base + total > 0xFEE00000ULL) ||
        KldIoApic::OwnsRange(base, total)) {
        SerialLogger::Log("[kldb] acpi MAP_PHYS refused: kernel mmio 0x");
        SerialLogger::LogHex64(base);
        SerialLogger::Log("\r\n");
        return KLD_EPERM;
    }
    if (overlaps_pci_bar(base, total) || overlaps_ecam(base, total) ||
        overlaps_drhd(base, total) || overlaps_hpet(base, total) ||
        ov(base, total, TPM_LO, TPM_HI - TPM_LO)) {
        SerialLogger::Log("[kldb] acpi MAP_PHYS refused: pci/ecam/drhd/hpet 0x");
        SerialLogger::LogHex64(base);
        SerialLogger::Log("\r\n");
        return KLD_EPERM;
    }
    if (mapped_elsewhere(as, base, total)) {
        SerialLogger::Log("[kldb] acpi MAP_PHYS refused: mapped by another container 0x");
        SerialLogger::LogHex64(base);
        SerialLogger::Log("\r\n");
        return KLD_EBUSY;
    }
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
    Container* c = find_container(as, false, -1);
    if (!c) return KLD_EPERM;   // no record: refuse, never map unrecorded (satoru)
    if (c->acpi_map_pages + pages > 4096) {   // 16 MiB of opregion windows (satoru)
        SerialLogger::Log("[kldb] acpi MAP_PHYS refused: 16 MiB map budget\r\n");
        return KLD_ENOMEM;
    }
    MapRec* m = nullptr;
    for (int i = 0; i < MAX_MAPS; i++)
        if (!c->maps[i].used) { m = &c->maps[i]; break; }
    if (!m) return KLD_ENOSPC;
    if (!va_range_free(as, va, pages)) return KLD_EBUSY;
    for (uint64_t i = 0; i < pages; i++) {
        PMM::RetainFrame(base + i * PAGE_SIZE);
        PMM::RetainFrame(base + i * PAGE_SIZE);
    }
    // acpi reclaim/nvs is ordinary memory (write-back); anything else is
    // treated as mmio (uncached). a driver family maps opregions READ-ONLY
    // unless the owner allowlisted the range at boot: acpica's _INI osys write
    // is then refused by the kernel's page tables, which is the point. (satoru)
    bool ro = acpi_strict(as) && !allow_mem(base, total);
    uint64_t fl = PTE_PRESENT | PTE_USER | PTE_NX;
    if (!ro) fl |= PTE_WRITABLE;
    if (!all_acpi) fl |= PTE_PCD | PTE_PWT;
    if (!map_pages(as, va, base, pages, fl)) return KLD_ENOMEM;
    if (m) {
        m->used = true;
        m->plat = false;
        m->va = va;
        m->phys = base;
        m->pages = (uint32_t)pages;
        c->acpi_map_pages += (uint32_t)pages;
    }
    SerialLogger::Log("[kldb] acpi map phys 0x");
    SerialLogger::LogHex64(base);
    SerialLogger::Log(ro ? " (ro, strict family)" : " (rw)");
    SerialLogger::Log("\r\n");
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

// writes refused for EVERY acpi container, strict family or the kernel's own
// runner, allowlist or not: SMI_CMD and the APM port pair 0xB2/0xB3, the fast
// A20/reset port 0x92, the reset control 0xCF9, cmos index/data 0x70-0x73, the
// FADT's SMI_CMD and PM1 control blocks. a strict family can still READ any
// port that is not in port_denied(): that includes 0x71/0x73 (cmos data, no
// side effect without an index write), 0xB2/0xB3 (apm status), 0x92, the PM1
// status/enable and GPE blocks (reads do not clear), and the EC ports. reads
// with side effects that remain: the 8259/PIT/8042 are denied above; a read of
// the PM timer or an EC status byte has none. (satoru)
bool port_write_denied(uint32_t port, uint32_t width) {
    uint32_t end = port + width / 8;
    struct { uint32_t lo, hi; } deny[] = {
        { 0x0070, 0x0074 }, { 0x00B2, 0x00B4 }, { 0x0092, 0x0093 }, { 0x0CF9, 0x0CFA },
    };
    for (auto& d : deny)
        if (port < d.hi && end > d.lo) return true;
    if (g_fadt_smi_cmd && port < g_fadt_smi_cmd + 1u && end > g_fadt_smi_cmd) return true;
    uint32_t cl = g_fadt_pm1_cnt_len ? g_fadt_pm1_cnt_len : 2u;
    if (g_fadt_pm1a_cnt && port < g_fadt_pm1a_cnt + cl && end > g_fadt_pm1a_cnt) return true;
    if (g_fadt_pm1b_cnt && port < g_fadt_pm1b_cnt + cl && end > g_fadt_pm1b_cnt) return true;
    return false;
}

int64_t op_acpi_port(uint64_t as, uint32_t port, uint32_t width, bool out, uint32_t val) {
    if (width != 8 && width != 16 && width != 32) return KLD_EINVAL;
    if (port > 0xFFFF) return KLD_EINVAL;
    Container* c = acpi_container(as);
    bool strict = acpi_strict(as);
    if (out) {
        bool denied = port_denied(port, width) || port_write_denied(port, width);
        bool allowed = allow_port(port, width);
        if (denied || (strict && !allowed)) {
            // refused at the hardware, visible to the caller's read-back. a
            // full per-container shadow is a refusal, never a silent drop. (satoru)
            if (!shadow_put(as, false, port, width, val)) {
                SerialLogger::Log("[kldb] acpi port write refused, shadow FULL\r\n");
                return KLD_EPERM;
            }
            if (c) c->acpi_shadow_port_w++;
            return KLD_OK;
        }
        if (width == 8) HAL::OutByte((uint16_t)port, (uint8_t)val);
        else if (width == 16) HAL::OutWord((uint16_t)port, (uint16_t)val);
        else HAL::OutLong((uint16_t)port, val);
        if (c) c->acpi_real_port_w++;
        return KLD_OK;
    }
    uint32_t sv;
    if (shadow_get(as, false, port, width, &sv)) return (int64_t)sv;
    if (strict) log_port_read_once(port);
    // the read denylist applies to EVERYONE. strict only adds restrictions
    // (a read of the 8042 steals a key byte; the pic/pit/cmos ports are the
    // kernel's). (satoru)
    if (port_denied(port, width)) return KLD_EPERM;
    if (width == 8) return HAL::InByte((uint16_t)port);
    if (width == 16) return HAL::InWord((uint16_t)port);
    return (int64_t)HAL::InLong((uint16_t)port);
}

int64_t op_acpi_pci(uint64_t as, uint32_t bdf, uint32_t off, uint32_t width, bool write, uint32_t val) {
    if (!bdf_valid(bdf) || off > 0xFF) return KLD_EINVAL;
    if (width != 1 && width != 2 && width != 4) return KLD_EINVAL;
    Container* c = acpi_container(as);
    bool strict = acpi_strict(as);
    uint32_t key = (bdf << 8) | off;
    if (!write) {
        uint32_t sv;
        if (shadow_get(as, true, key, width, &sv)) return (int64_t)sv;
        // AML legitimately reads device config (the _PRT/ACPI enumerator), so
        // the read is allowed; every distinct device is logged once. (satoru)
        if (strict) log_pci_read_once(bdf);
        return (int64_t)cfg_read(bdf, off, width);
    }
    bool refused = cfg_write_refused(bdf, off, width);
    bool allowed = allow_pci(bdf, off, width);
    if (refused || (strict && !allowed)) {
        if (!shadow_put(as, true, key, width, val)) {
            log_bdf("[kldb] acpi pci write refused, shadow FULL ", bdf);
            SerialLogger::Log("\r\n");
            return KLD_EPERM;
        }
        if (c) c->acpi_shadow_pci_w++;
        return KLD_OK;
    }
    cfg_write(bdf, off, width, val);
    if (c) c->acpi_real_pci_w++;
    return KLD_OK;
}

}  // namespace
// public display-instance API (definitions live in the anonymous namespace
// above; these are the KldBroker:: symbols the rest of the kernel calls). (satoru)
int64_t AttachDisplayInstance(uint64_t as, int pid, uint64_t ring_va,
                              uint64_t scanout_va, uint32_t ring_pages,
                              uint32_t scanout_pages, uint32_t* id_out)
{
    return dispint_attach(as, pid, ring_va, scanout_va, ring_pages,
                          scanout_pages, id_out);
}
void DisplayExit(uint32_t id, bool as_dead)
{
    dispint_exit(id, as_dead);
}
bool MarkDisplayPublished(uint32_t id) { return dispint_published(id); }
bool MarkDisplayDying(uint32_t id) { return dispint_dying(id); }
bool ReleaseDisplayInstance(uint32_t id) { return dispint_release(id); }
bool DisplayAttachBusy(uint64_t as) { return dispint_busy(as); }
bool KillDisplayInstance(uint32_t id)
{
    int pid = 0;
    {
        SpinLockGuard g(g_disp_lock);
        DispInst* r = disp_find_locked(id);
        if (!r) return false;
        pid = r->pid;
    }
    if (pid > 0)
        Scheduler::KillProcessGroupAsync((uint32_t)pid, nullptr);
    return true;
}

namespace {
bool starts_with(const char* p, const char* s)
{
    while (*s) { if (*p++ != *s++) return false; }
    return true;
}

uint64_t ahex64(const char*& p)
{
    while (*p == ' ' || *p == '\t') p++;
    uint64_t v = 0;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    while (1) {
        char c = *p;
        if (c >= '0' && c <= '9') v = v * 16 + (uint64_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v = v * 16 + (uint64_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = v * 16 + (uint64_t)(c - 'A' + 10);
        else break;
        p++;
    }
    return v;
}
}  // namespace

void SetAcpiAllow(const char* spec)
{
    g_acpi_allow_n = 0;
    const char* p = spec ? spec : "";
    while (*p && g_acpi_allow_n < MAX_ACPI_ALLOW) {
        while (*p == ',' || *p == ' ') p++;
        if (!*p) break;
        AcpiAllow e = {};
        if (starts_with(p, "port:")) {
            p += 5;
            e.kind = 0;
            e.a = ahex64(p);
            if (*p == ':') p++;
            e.b = ahex64(p);
        } else if (starts_with(p, "pci:")) {
            p += 4;
            uint64_t bus = ahex64(p);
            if (*p == ':') p++;
            uint64_t dev = ahex64(p);
            if (*p == '.') p++;
            uint64_t fn = ahex64(p);
            if (*p == ':') p++;
            e.kind = 1;
            e.a = ((uint32_t)bus << 16) | ((uint32_t)dev << 8) | (uint32_t)fn;
            e.b = ahex64(p);
            if (*p == ':') p++;
            e.c = ahex64(p);
        } else if (starts_with(p, "mem:")) {
            p += 4;
            e.kind = 2;
            uint64_t ph = ahex64(p);
            if (*p == ':') p++;
            uint64_t len = ahex64(p);
            e.a = (uint32_t)ph;
            e.b = (uint32_t)len;
            if (ph > 0xFFFFFFFFull || len > 0xFFFFFFFFull) {
                SerialLogger::Log("[kldb] acpi allow: mem entry above 4 GiB ignored\r\n");
                while (*p && *p != ',') p++;
                continue;
            }
        } else {
            while (*p && *p != ',') p++;
            continue;
        }
        g_acpi_allow[g_acpi_allow_n++] = e;
        SerialLogger::Log("[kldb] acpi allow entry kind ");
        SerialLogger::LogDec(e.kind);
        SerialLogger::Log(" a 0x");
        SerialLogger::LogHex(e.a);
        SerialLogger::Log(" b 0x");
        SerialLogger::LogHex(e.b);
        SerialLogger::Log("\r\n");
        while (*p && *p != ',') p++;
    }
    if (!g_acpi_allow_n)
        SerialLogger::Log("[kldb] acpi allowlist empty: port/pci writes refused at the hardware\r\n");
}

const char* FamilyOfAddressSpace(uint64_t address_space) {
    Container* c = find_container(address_space, false, -1);
    return (c && c->family[0]) ? c->family : "";
}

// true when [va, va+len) overlaps a broker-installed user mapping (MAP_BAR,
// DMA_ALLOC, IRQ_PAGE, NET_ATTACH, DISPLAY_ATTACH). those ptes are the
// kernel's, not the container's: the linux layer must refuse munmap, madvise,
// mremap, MAP_FIXED and mprotect over them, and the fault handler must not
// re-apply a region's writable flags to a read-only broker page. (satoru)
bool RangeIsBrokerMapping(uint64_t address_space, uint64_t va, uint64_t len)
{
    if (!len) return false;
    uint64_t hi = va + len;
    if (hi < va) hi = ~0ull;
    if (KldDri::RangeIsContainerMapping(address_space, va, len)) return true;   // drm ring + object windows (satoru)
    Container* c = find_container(address_space, false, -1);
    if (!c || c->as_gone) return false;
    auto hit = [&](uint64_t lo, uint64_t bytes) {
        if (!bytes) return false;
        uint64_t end = lo + bytes;
        if (end < lo) end = ~0ull;
        return va < end && lo < hi;
    };
    for (int i = 0; i < MAX_MAPS; i++)
        if (c->maps[i].used &&
            hit(c->maps[i].va, (uint64_t)c->maps[i].pages * PAGE_SIZE))
            return true;
    for (int i = 0; i < MAX_DMA; i++)
        if (c->dma[i].used &&
            hit(c->dma[i].va, (uint64_t)c->dma[i].pages * PAGE_SIZE))
            return true;
    if (c->irq_page_va && hit(c->irq_page_va, PAGE_SIZE)) return true;
    if (c->net_va &&
        hit(c->net_va, (uint64_t)KldNet::KLD_NET_PAGES * PAGE_SIZE))
        return true;
    {
        SpinLockGuard g(g_alias_lock);
        for (int i = 0; i < MAX_ALIAS; i++)
            if (g_alias[i].used && g_alias[i].as == address_space &&
                g_alias[i].gen == c->gen &&
                hit(g_alias[i].va, (uint64_t)g_alias[i].reserved * PAGE_SIZE))
                return true;
    }
    {
        SpinLockGuard g(g_disp_lock);
        for (int i = 0; i < MAX_DISP_INST; i++) {
            DispInst* r = &g_disp_inst[i];
            if (!r->used || r->as != address_space || r->state == DISP_RELEASED)
                continue;
            if (hit(r->ring_va, (uint64_t)r->ring_pages * PAGE_SIZE)) return true;
            if (hit(r->scan_va, (uint64_t)r->scan_pages * PAGE_SIZE)) return true;
        }
    }
    return false;
}

bool PhysIsUsableRam(uint64_t base, uint64_t len)
{
    if (!g_map_n || g_map_trunc) return true;   // no/partial map: unknown counts as unsafe (satoru)
    uint64_t end = base + len;
    for (uint64_t p = base & ~0xFFFULL; p < end; p += PAGE_SIZE)
        if (map_type(p) == 1) return true;
    return false;
}

void SetLegacyVgaForced(bool on) { g_legacy_vga_forced = on; }

// one broker allocation ref per frame, released after the gui ack. the
bool MsixTableFlagForGrant(const char* family, uint32_t vendor, uint32_t device,
                           uint32_t subvendor, uint32_t subdevice,
                           uint32_t class_code)
{
    if (!KldAuto::TablePresent()) {
        if (!g_logged_no_table) {
            g_logged_no_table = true;
            SerialLogger::Log("[kldb] no kld match table in this kernel\r\n");
        }
        return false;
    }
    if (!family || !family[0])
        return false;
    bool need = false;
    if (!KldAuto::FamilyMatchDeviceByName(family, vendor, device, subvendor,
                                          subdevice, class_code, &need))
        return false;
    return need;
}

// the auto-matcher must not walk the bus on its own: it asks for the one scan
// table every PCI_FIND already uses. (satoru)
int ScanDeviceCount() {
    scan_bus_once();
    return g_dev_count;
}

bool ScanDeviceAt(int idx, uint32_t* bdf, uint32_t* vendor, uint32_t* device,
                  uint32_t* subvendor, uint32_t* subdevice, uint32_t* class_code) {
    scan_bus_once();
    if (idx < 0 || idx >= g_dev_count) return false;
    if (bdf) *bdf = g_devs[idx].bdf;
    if (vendor) *vendor = g_devs[idx].vendor;
    if (device) *device = g_devs[idx].device;
    if (subvendor) *subvendor = g_devs[idx].subvendor;
    if (subdevice) *subdevice = g_devs[idx].subdevice;
    if (class_code) *class_code = g_devs[idx].class_code;
    return true;
}

void SetBarPolicyMode(int mode) { g_bar_mode = (mode >= 0 && mode <= 2) ? mode : 0; }

void Init() {
    KernelVMM::SetAddressSpaceDestroyHook2(dispint_as_destroyed);
    for (int i = 0; i < MAX_GRANTS; i++) g_grants[i].used = false;
    for (int i = 0; i < MAX_CONTAINERS; i++) g_cont[i].used = false;
    for (int i = 0; i < (int)MSI_VECTOR_COUNT; i++) {
        g_msi[i].used = false;
        g_msi[i].msix = false;
        g_line_gsi[i] = 0xFFFFFFFFu;   // no line ever attached to this vector (satoru)
        g_vec_in_flight[i] = 0;
        line_reset(i);
    }
    KldIoApic::Ensure();   // MADT read only; registers wait for an attach (satoru)
    build_refusal_ranges(); // once; containers may rewrite the live tables (satoru)
    SerialLogger::Log("[kldb] KLD broker initialized (pci proxy, bar map, dma, msi, line irq, net bridge)\r\n");
}

// the auto matcher's selected device for this container: from now on its
// PCI_FIND returns that bdf only (see op_pci_find). called once by the auto
// runner before the container runs. (satoru)
void FreeAfterTlbProof(uint64_t phys, uint64_t pages, uint32_t refs) {
    free_after_tlb_proof(phys, pages, refs);
}

void DrainTlbProven(bool buy_proof) { tlbq_drain(buy_proof); }

void SetTlbSkipTest(bool on) { g_tlbskip_armed = on; }

bool SetPreferredBdf(uint64_t as, uint32_t bdf) {
    Container* c = find_container(as, false, -1);
    if (!c) return false;   // the runner must not start the container (satoru)
    c->pref_bdf = bdf;
    c->pref_bdf_valid = true;
    return true;
}

void SetDmaBudget(uint64_t as, uint32_t bytes, int pid) {
    uint32_t pages = bytes / PAGE_SIZE;
    if (pages < 1) pages = 1;
    if (pages > 65536) pages = 65536;   // 256 MiB ceiling
    Container* c = find_container(as, true, pid);
    if (c) {
        c->dma_limit_pages = pages;
        c->as_gone = false;   // a new container owns this key again (satoru)
    }
    SerialLogger::Log("[kldb] dma budget ");
    SerialLogger::LogDec((int)(pages / 256));   // MiB
    SerialLogger::Log(" MiB\r\n");
}

void SetDmaBudgetForName(uint64_t as, const char* name, int pid) {
    static const char needle[] = "iwlwifi";
    Container* c = find_container(as, true, pid);
    if (c) {
        family_from_path(name, c->family, (int)sizeof(c->family));
        c->as_gone = false;   // a new container owns this key again (satoru)
    }
    bool wifi = false;
    if (name) {
        for (const char* p = name; *p && !wifi; p++) {
            int i = 0;
            while (needle[i] && p[i] == needle[i]) i++;
            if (!needle[i]) wifi = true;
        }
    }
    SetDmaBudget(as, wifi ? (256u << 20) : (64u << 20), pid);
}



void SetIrqWake(bool on) { g_irq_wake = on; }

void SetLineAllow(const char* spec)
{
    g_line_allow_n = 0;
    for (const char* p = spec ? spec : ""; *p && g_line_allow_n < 8;) {
        while (*p == ',' || *p == ' ') p++;
        if (*p < '0' || *p > '9') break;
        uint32_t v = 0;
        while (*p >= '0' && *p <= '9') v = v * 10 + (uint32_t)(*p++ - '0');
        g_line_allow[g_line_allow_n++] = v;
    }
}

void SetNoMsi(bool on)
{
    g_nomsi = on;
    if (on) SerialLogger::Log("[kldb] msi/msix disabled for this boot (kurono.kld.nomsi=1)\r\n");
}

void SnapshotMemoryMap(const void* mbi_v) {
    const multiboot_info_t* mbi = (const multiboot_info_t*)mbi_v;
    g_map_n = 0;
    if (!mbi || !(mbi->flags & (1u << 6))) return;
    const uint8_t* p = (const uint8_t*)(uintptr_t)mbi->mmap_addr;
    const uint8_t* end = p + mbi->mmap_length;
    g_map_trunc = false;
    while (p + 24 <= end) {
        if (g_map_n >= MAP_MAX) { g_map_trunc = true; break; }
        uint32_t sz = *(const uint32_t*)p;
        g_map[g_map_n].base = *(const uint64_t*)(p + 4);
        g_map[g_map_n].len  = *(const uint64_t*)(p + 12);
        g_map[g_map_n].type = *(const uint32_t*)(p + 20);
        g_map_n++;
        p += sz + 4;
    }
    SerialLogger::Log("[kldb] firmware memory map: ");
    SerialLogger::LogDec(g_map_n);
    SerialLogger::Log(" entries");
    SerialLogger::Log(g_map_trunc ? ", TRUNCATED: plat/acpi maps are refused\r\n" : "\r\n");
}

void GrantAcpi(uint64_t address_space) {
    g_acpi_as = address_space;
    SerialLogger::Log(address_space ? "[kldb] acpi privilege granted to the kld-acpi container\r\n"
                                    : "[kldb] acpi privilege revoked\r\n");
}

uint64_t N1bIrqs() { return __atomic_load_n(&g_n1b_irqs, __ATOMIC_ACQUIRE); }
uint64_t N1bWakes() { return __atomic_load_n(&g_n1b_wakes, __ATOMIC_ACQUIRE); }
void SetStats(bool on) { g_kldstats = on; }
bool StatsOn() { return __atomic_load_n(&g_kldstats, __ATOMIC_RELAXED); }
uint64_t N1cCalls() { return __atomic_load_n(&g_n1c_calls, __ATOMIC_ACQUIRE); }
uint64_t N1cFound() { return __atomic_load_n(&g_n1c_found, __ATOMIC_ACQUIRE); }
uint64_t N1cNone() { return __atomic_load_n(&g_n1c_none, __ATOMIC_ACQUIRE); }
uint64_t N1cNotRun() { return __atomic_load_n(&g_n1c_notrun, __ATOMIC_ACQUIRE); }

void OnVector(uint32_t vector) {
    uint32_t h = vector - MSI_VECTOR_BASE;
    if (h >= MSI_VECTOR_COUNT) return;
    // kurono.kld.stats off: the N1b/N1c instrumentation below is one flag test
    // (this load) and nothing else. (satoru)
    const bool st = __atomic_load_n(&g_kldstats, __ATOMIC_RELAXED);
    uint64_t t0 = st ? n1b_rdtsc() : 0;
    __atomic_fetch_add(&g_vec_in_flight[h], 1u, __ATOMIC_ACQ_REL);
    g_vec_hits[h]++;
    if (st) __atomic_fetch_add(&g_n1b_irqs, 1, __ATOMIC_RELAXED);
    MsiRec* m = &g_msi[h];
    if (!__atomic_load_n(&m->used, __ATOMIC_ACQUIRE)) {
        // an ownerless delivery (detach won the race): mask the known line so
        // the bsp never re-enters this vector for a line nobody owns. (satoru)
        uint32_t g = g_line_gsi[h];
        bool other = false;   // never mask a line another slot owns now (satoru)
        for (uint32_t i = 0; i < MSI_VECTOR_COUNT && g != 0xFFFFFFFFu; i++)
            if (i != h && __atomic_load_n(&g_msi[i].used, __ATOMIC_ACQUIRE) &&
                g_msi[i].line && g_msi[i].gsi == g)
                other = true;
        if (g != 0xFFFFFFFFu && !other)   // gsi 0 is valid; the sentinel is "never" (satoru)
            KldIoApic::Mask(g);
        __atomic_fetch_sub(&g_vec_in_flight[h], 1u, __ATOMIC_ACQ_REL);
        return;
    }
    uint64_t now_ms = 0;
    N1cSnap snap[24];
    int nb0 = 0;
    if (st) {
        now_ms = Timer::GetRealMs64();
        // check the PREVIOUS wake: if its thread still has not run (same
        // save_seq, not Running), it sat ready past the 500 ms bar. (satoru)
        if (g_n1c_prev_ok) {
            Process* p = Scheduler::FindProcessByPid(g_n1c_prev.pid);
            if (p && now_ms > g_n1c_prev.t0_ms && now_ms - g_n1c_prev.t0_ms > 500) {
                bool ran = (p->save_seq != g_n1c_prev.save_seq) || (p->state == Process_Running);
                if (!ran) {
                    g_n1c_notrun++;
                    if (now_ms - g_n1c_last_print_ms > 2000) {
                        bool i0 = false, i1 = false;
                        int nc = (int)SMP::CpuCount();
                        const char* n0 = nc > 0 ? n1c_cpu_task(0, &i0) : "none";
                        const char* n1 = nc > 1 ? n1c_cpu_task(1, &i1) : "none";
                        g_n1c_last_print_ms = now_ms;
                        SerialLogger::Log("[lx_n1c] long hop ");
                        SerialLogger::LogDec((int)(now_ms - g_n1c_prev.t0_ms));
                        SerialLogger::Log("ms: thread was Ready and not run (");
                        SerialLogger::Log(p->name);
                        SerialLogger::Log(" pid=");
                        SerialLogger::LogDec((int)p->pid);
                        SerialLogger::Log(" state=");
                        SerialLogger::LogDec((int)p->state);
                        SerialLogger::Log(" on_cpu=");
                        SerialLogger::LogDec((int)p->on_cpu);
                        SerialLogger::Log(" last_run_cpu=");
                        SerialLogger::LogDec((int)p->last_run_cpu);
                        SerialLogger::Log(" ready_since=");
                        SerialLogger::LogDec((int)p->ready_since_ms);
                        SerialLogger::Log(") queued_cpu=none");
                        SerialLogger::Log(" c0=");
                        SerialLogger::Log(n0);
                        SerialLogger::Log(i0 ? "(idle)" : "");
                        SerialLogger::Log(" c1=");
                        SerialLogger::Log(n1);
                        SerialLogger::Log(i1 ? "(idle)" : "");
                        SerialLogger::Log("\r\n");
                    }
                }
            }
            g_n1c_prev_ok = false;
        }
        // snapshot before the forward: Blocked + then not Blocked = found. (satoru)
        nb0 = n1c_blocked_snapshot(m->as, snap, 24);
    }
    if (m->line) {
        // a level line is masked on delivery (the irq thread unmasks it after
        // the handler); an edge line must stay unmasked or it loses edges. the
        // mmio writes are safe in isr context: the ioapic lock is irqsave.
        // (satoru)
        if (m->line_flags & KldIoApic::LINE_LEVEL) {
            KldIoApic::Mask(m->gsi);
            // suppression set at attach (SVR bit 12): the local EOI does not
            // clear this level entry's remote irr, write the ioapic EOIR.
            // otherwise the local EOI broadcasts and the ioapic is untouched.
            // (satoru)
            if (m->eoi_suppress)
                KldIoApic::Eoi(m->gsi, vector);
        }
        // isr-side budget the container cannot game: over 512 deliveries in a
        // 100 ms window the line is masked until the window rolls over. (satoru)
        uint64_t now = Scheduler::NowMs();
        if (now - m->rate_ms >= 100) {
            m->rate_ms = now;
            m->rate_count = 0;
        }
        if (++m->rate_count > 512) {
            if (!m->rate_masked) {
                m->rate_masked = true;
                SerialLogger::Log("[kldb] line gsi ");
                SerialLogger::LogDec((int)m->gsi);
                SerialLogger::Log(" rate: >512 deliveries in 100ms, masked to the next window\r\n");
            }
            KldIoApic::Mask(m->gsi);
        }
    }
    m->fired++;
    // the doorbell page is alive while this vector is in flight: release waits
    // for g_vec_in_flight[h] to drain before it frees the page, and a record
    // whose used flag was cleared returns above. (satoru)
    if (st && m->t0slot)
        __atomic_store_n(m->t0slot, t0, __ATOMIC_RELEASE);
    if (st) {
        // tag for the container: 1 = a blocked waiter existed at this irq
        // (wake should find it), 2 = none (thread was running / not waiting).
        volatile uint32_t* tag = (volatile uint32_t*)PMM::PhysToVirt(m->counter_phys + 3072);
        __atomic_store_n(tag, nb0 ? 1u : 2u, __ATOMIC_RELEASE);
    }
    __atomic_fetch_add(m->counter, 1u, __ATOMIC_RELEASE);
    if (g_irq_wake) {
        if (st) __atomic_fetch_add(&g_n1b_wakes, 1, __ATOMIC_RELAXED);
        LinuxSyscallFutexWakePhys(m->counter_phys, 0x7FFFFFFF);
    }
    if (st) {
        g_n1c_calls++;
        Process* woken = nullptr;
        for (int i = 0; i < nb0; i++) {
            Process* p = Scheduler::FindProcessByPid(snap[i].pid);
            if (p && p->state != Process_Blocked) {
                woken = p;
                g_n1c_prev.save_seq = snap[i].seq;
                break;
            }
        }
        if (woken) {
            g_n1c_found++;
            g_n1c_prev.pid = woken->pid;
            g_n1c_prev.t0_ms = now_ms;
            g_n1c_prev.state = (uint8_t)Process_Blocked;
            g_n1c_prev_ok = true;
        } else {
            g_n1c_none++;
        }
    }
    __atomic_fetch_sub(&g_vec_in_flight[h], 1u, __ATOMIC_ACQ_REL);
}

// true when this address space was a container's and that address space was
// destroyed, and no live record has claimed the key since: the caller is a
// later process on a reused PML4 frame. (satoru)
static bool stale_as(uint64_t as)
{
    if (find_container(as, false, -1)) return false;
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (g_cont[i].used && g_cont[i].as == as && g_cont[i].as_gone) return true;
    for (int i = 0; i < MAX_GRANTS; i++)
        if (g_grants[i].used && g_grants[i].as == as && g_grants[i].dead) return true;
    return false;
}

static uint32_t g_stale_refused;

static int64_t dispatch(uint64_t as, int pid, const char* path, uint32_t op,
                 uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    // every op except the one that registers a fresh container is refused
    // for a stale address space, including the ones keyed by address space
    // outside this file (display, net, supervisor). (satoru)
    tlbq_drain(false);   // proven frees, no new broadcast here (satoru)
    if (op != KLD_OP_PCI_FIND && stale_as(as)) {
        if (g_stale_refused++ < 8) {
            SerialLogger::Log("[kldb] op ");
            SerialLogger::LogHex(op);
            SerialLogger::Log(" from as=0x");
            SerialLogger::LogHex64(as);
            SerialLogger::Log(", the address space of a dead container: refused\r\n");
        }
        return KLD_EPERM;
    }
    switch (op) {
        case KLD_OP_PCI_FIND:
            return op_pci_find(as, pid, (uint32_t)a0, (uint32_t)a1, (uint32_t)a2, path);
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
        case KLD_OP_TSC_HZ:
            return (int64_t)CPUDetect::GetInfo().frequency.tsc_frequency;
        case KLD_OP_DMA_ALIAS:
            return op_dma_alias(as, a0, a1, (uint32_t)a2, a3 != 0);   // scalars only (satoru)
        case KLD_OP_DMA_UNALIAS:
            return op_dma_unalias(as, a0);
        case KLD_OP_OPREGION_MAP:
            return op_opregion_map(as, (uint32_t)a1, a0);
        case KLD_OP_HOST_CFG_READ:
            return op_host_cfg_read(as, (uint32_t)a0, (uint32_t)a1);
        case KLD_OP_GT_WRITE:
            return op_gt_write(as, (uint32_t)a0, (uint32_t)a1, a2);
        case KLD_OP_PORT_IN:
            return op_port_in(as, a0, a1);
        case KLD_OP_PORT_OUT:
            return op_port_out(as, a0, a1, a2);
        case KLD_OP_DISPLAY_ATTACH: {
            uint32_t id = 0;
            return AttachDisplayInstance(as, pid, a0, a1, a2, a3, &id);
        }
        case KLD_OP_DISPLAY_MODES:
            return KldDisplay::ReportModes(as);
        case KLD_OP_DISPLAY_DONE:
            return KldDisplay::Done(as, (uint32_t)a0);
        case KLD_OP_DISPLAY_HOTPLUG:
            return KldDisplay::Hotplug(as, (uint32_t)a0);
        case KLD_OP_DISPLAY_ERROR:
            return KldDisplay::Error(as, (uint32_t)a0);
        case KLD_OP_DISPLAY_DETACH: {
            Container* dc = find_container(as, false, pid);
            if (!dc) return KLD_EPERM;
            uint32_t did = 0;
            {
                SpinLockGuard g(g_disp_lock);
                did = dc->disp_id;
            }
            // an explicit detach by a live container: the address space is
            // alive, so the final free must unmap and return the mapping ref.
            // (satoru)
            if (did) dispint_exit(did, false);
            return KLD_OK;
        }
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
        case KLD_OP_IRQ_LINE_ATTACH:
            return op_irq_line_attach(as, (uint32_t)a0, (uint32_t)a1);
        case KLD_OP_IRQ_LINE_UNMASK:
            return op_irq_line_unmask(as, (uint32_t)a0, (uint32_t)a1);
        case KLD_OP_IRQ_LINE_DETACH:
            return op_irq_line_detach(as, (uint32_t)a0);
        case KLD_OP_PLAT_MAP:
            return op_plat_map(as, a0, a1, a2);
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
            return op_acpi_port(as, (uint32_t)a0, (uint32_t)a1, false, 0);
        case KLD_OP_ACPI_PORT_OUT:
            if (!is_acpi(as)) return KLD_EPERM;
            return op_acpi_port(as, (uint32_t)a0, (uint32_t)a1, true, (uint32_t)a2);
        case KLD_OP_ACPI_PCI_READ:
            if (!is_acpi(as)) return KLD_EPERM;
            return op_acpi_pci(as, (uint32_t)a0, (uint32_t)a1, (uint32_t)a2, false, 0);
        case KLD_OP_INPUT_POINTER: {
            Container* c = find_container(as, false, pid);
            const char* fam = c ? c->family : nullptr;
            bool ok = false;
            auto exact = [](const char* a, const char* b) {
                while (*a && *a == *b) { a++; b++; }
                return *a == 0 && *b == 0;
            };
            if (fam && (exact(fam, "i2chid") || exact(fam, "i2chidsim") || exact(fam, "hidsim")))
                ok = true;
            if (!ok)
                return KLD_EPERM;
            KldInput::Pointer((int)a0, (int)a1, (uint32_t)a2, (int)a3);
            return KLD_OK;
        }
        case KLD_OP_ACPI_PCI_WRITE:
            if (!is_acpi(as)) return KLD_EPERM;
            return op_acpi_pci(as, (uint32_t)a0, (uint32_t)a1, (uint32_t)a2, true, (uint32_t)a3);
        case KLD_OP_TEST_CANARY: {
            // test only, gated inside KldIommu by kurono.kld.iommutest=1.
            // the container asks before its pci_find, so no grant yet. (satoru)
            uint64_t canary = 0;
            if (!KldIommu::TestCanary(&canary)) return KLD_EPERM;
            return (int64_t)canary;
        }
        case KLD_OP_STATUS: {
            // a0=1+h: raw isr delivery count of vector h, for diagnostics. (satoru)
            if (a0 >= 1 && a0 <= MSI_VECTOR_COUNT) return (int64_t)g_vec_hits[a0 - 1];
            int n = 0;
            for (int i = 0; i < MAX_GRANTS; i++)
                if (grant_live(g_grants[i], as)) n++;
            return (int64_t)n;
        }
        case KLD_OP_HEARTBEAT:
            KldSupervisor::Heartbeat(as);
            return KLD_OK;
        case KLD_OP_RELEASE: {
            Container* c = find_container(as, false, pid);
            if (c) release_container(c);
            return KLD_OK;
        }
        default:
            return KLD_EINVAL;
    }
}

// kurono.kld.asgonetest=1: a container record and a grant on a real address
// space, the destroy hook's marking, then every op as an ordinary process on
// that (reused) address space would issue it. the address space stays alive
// through the ops so a missed lookup could not touch a freed table. (satoru)
void AsGoneSelfTest() {
    uint64_t as = KernelVMM::CreateAddressSpace();
    if (!as) { SerialLogger::Log("[kldb] asgonetest FAIL no address space\r\n"); return; }
    const uint32_t bdf = 0x00FF1F07;   // bus ff dev 1f fn 7: no device (satoru)
    int fails = 0;
    SetDmaBudgetForName(as, "/usr/bin/kld-asgonetest", 0);
    add_grant(as, 0, bdf);
    // positive control: the live record and grant answer. (satoru)
    if (dispatch(as, 0, nullptr, KLD_OP_STATUS, 0, 0, 0, 0) != 1) fails++;
    if (dispatch(as, 0, nullptr, KLD_OP_PCI_CFG_READ, bdf, 0, 2, 0) == KLD_EPERM) fails++;
    int live_fails = fails;
    dispint_as_destroyed(as);   // what the vmm destroy hook does (satoru)
    static const uint32_t ops[] = {
        KLD_OP_PCI_CFG_READ, KLD_OP_PCI_CFG_WRITE, KLD_OP_PCI_BAR, KLD_OP_PCI_BAR_SIZE,
        KLD_OP_PCI_ENABLE, KLD_OP_PCI_DISABLE, KLD_OP_MAP_BAR, KLD_OP_DMA_ALLOC,
        KLD_OP_DMA_FREE, KLD_OP_DMA_ALIAS, KLD_OP_DMA_UNALIAS, KLD_OP_PORT_IN,
        KLD_OP_PORT_OUT, KLD_OP_DISPLAY_ATTACH, KLD_OP_DISPLAY_MODES,
        KLD_OP_DISPLAY_DONE, KLD_OP_DISPLAY_HOTPLUG, KLD_OP_DISPLAY_ERROR,
        KLD_OP_DISPLAY_DETACH, KLD_OP_IRQ_PAGE, KLD_OP_MSI_ALLOC, KLD_OP_MSI_FREE,
        KLD_OP_MSIX_ALLOC, KLD_OP_MSIX_FREE, KLD_OP_MSIX_COUNT, KLD_OP_NET_ATTACH,
        KLD_OP_NET_CARRIER, KLD_OP_NET_RX_KICK, KLD_OP_NET_TICK, KLD_OP_STATUS,
        KLD_OP_HEARTBEAT, KLD_OP_RELEASE,
    };
    int n = 0, eperm = 0;
    for (uint32_t op : ops) {
        int64_t rc = dispatch(as, 0, nullptr, op, bdf, 0x10000000ull, 1, 0);
        n++;
        if (rc == KLD_EPERM) eperm++;
        else {
            fails++;
            SerialLogger::Log("[kldb] asgonetest op ");
            SerialLogger::LogHex(op);
            SerialLogger::Log(" not refused\r\n");
        }
    }
    // the lookups themselves, under the gate: no grant, no container, no
    // broker range, and the device is still held against other spaces until
    // the release. (satoru)
    if (find_grant(as, bdf)) fails++;
    if (find_container(as, false, -1)) fails++;
    if (op_cfg_read(as, bdf, 0, 2) != KLD_EPERM) fails++;
    if (RangeIsBrokerMapping(as, 0x10000000ull, PAGE_SIZE)) fails++;
    if (!granted_elsewhere(as, bdf)) fails++;
    ReleaseAddressSpace(as);
    int left = 0;
    for (int i = 0; i < MAX_CONTAINERS; i++) if (g_cont[i].used && g_cont[i].as == as) left++;
    for (int i = 0; i < MAX_GRANTS; i++) if (g_grants[i].used && g_grants[i].as == as) left++;
    if (left) fails++;
    KernelVMM::DestroyAddressSpace(as);
    SerialLogger::Log("[kldb] asgonetest live_fails=");
    SerialLogger::LogDec(live_fails);
    SerialLogger::Log(" ops=");
    SerialLogger::LogDec(n);
    SerialLogger::Log(" eperm=");
    SerialLogger::LogDec(eperm);
    SerialLogger::Log(" records_left=");
    SerialLogger::LogDec(left);
    SerialLogger::Log(fails ? " FAIL\r\n" : " PASS\r\n");
}

int64_t Call(Process* cur, uint32_t op, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    if (!cur || !cur->is_user() || !cur->address_space) return KLD_EPERM;
    // an untrusted container can reach real hardware through this broker, so
    // only a privileged process (uid 0 / approved) may call it. (satoru)
    if (!AdminGate::Authorize("hardware/driver access", "kld broker call")) return KLD_EPERM;
    if (op >= 0xA0 && op < 0xC0) {   // drm userspace interface; a reused PML4 of a dead container is refused like every op (satoru)
        if (stale_as(cur->address_space)) return KLD_EPERM;
        return KldDri::Call(cur, op, a0, a1, a2, a3);
    }
    return dispatch(cur->address_space, (int)cur->pid,
                    cur->exe_path[0] ? cur->exe_path : cur->name, op, a0, a1, a2, a3);
}

uint32_t ContainerId(uint64_t address_space) {
    // the runner identifies the broker slot after the process is gone, so a
    // dead (as_gone) record must still answer. (satoru)
    Container* c = find_container_release(address_space);
    return c ? c->gen : 0;
}

bool ReleaseInstance(uint64_t address_space, uint32_t id) {
    Container* c = find_container_release(address_space);
    if (!c || id == 0 || c->gen != id) return false;
    release_container(c);
    return true;
}

void ReleaseAddressSpace(uint64_t address_space) {
    Container* c = find_container_release(address_space);
    if (c) {
        release_container(c);   // frees exactly that record's grants (satoru)
        return;
    }
    // grants without a container record (find only). (satoru)
    for (int i = 0; i < MAX_GRANTS; i++)
        if (g_grants[i].used && g_grants[i].as == address_space)
            g_grants[i].used = false;
}

uint32_t CfgRead32Locked(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    SpinLockGuard g(g_cfg_lock);
    return PCI::Read32(bus, dev, fn, (uint8_t)(off & 0xFC));
}

bool GrantInfo(uint64_t address_space, uint32_t* bdf, uint16_t* vendor, uint16_t* device) {
    for (int i = 0; i < MAX_GRANTS; i++) {
        if (!grant_live(g_grants[i], address_space)) continue;
        uint32_t b = g_grants[i].bdf;
        if (bdf) *bdf = b;
        if (vendor) *vendor = (uint16_t)cfg_read(b, 0x00, 2);
        if (device) *device = (uint16_t)cfg_read(b, 0x02, 2);
        return true;
    }
    return false;
}

// the crash/release path: the container has exited, so its grants are
// normally already dead; name the device anyway (before the release). (satoru)
bool GrantInfoForRelease(uint64_t address_space, uint32_t* bdf, uint16_t* vendor,
                         uint16_t* device) {
    for (int i = 0; i < MAX_GRANTS; i++) {
        if (!g_grants[i].used || g_grants[i].as != address_space) continue;
        uint32_t b = g_grants[i].bdf;
        if (bdf) *bdf = b;
        if (vendor) *vendor = (uint16_t)cfg_read(b, 0x00, 2);
        if (device) *device = (uint16_t)cfg_read(b, 0x02, 2);
        return true;
    }
    return false;
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
