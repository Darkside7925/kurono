// KLD VT-d implementation. See kld_iommu.h and the M7 section of
// scratchpad/oc_kldg_report.md.
//
// Everything here is driven by the ACPI DMAR table and the capability
// registers; no register base, register offset or address width is assumed.
//
// (satoru)

#include "kld_iommu.h"
#include "../drivers/nvme.h"
#include "kld_broker.h"
#include "pmm.h"
#include "vmm.h"
#include "../drivers/serial.h"
#include "../proc/scheduler.h"
#include "../proc/smp.h"
#include "../proc/spinlock.h"

namespace KldIommu {

namespace {

// ── VT-d register set (Intel VT-d spec rev 4.0 chapter 11) ─────────────────
constexpr uint32_t REG_VER      = 0x000;
constexpr uint32_t REG_CAP      = 0x008;
constexpr uint32_t REG_ECAP     = 0x010;
constexpr uint32_t REG_GCMD     = 0x018;
constexpr uint32_t REG_GSTS     = 0x01C;
constexpr uint32_t REG_RTADDR   = 0x020;
constexpr uint32_t REG_CCMD     = 0x028;
constexpr uint32_t REG_FSTS     = 0x034;
constexpr uint32_t REG_FECTL    = 0x038;
constexpr uint32_t REG_PMEN     = 0x064;
constexpr uint32_t REG_IQH      = 0x080;
constexpr uint32_t REG_IQT      = 0x088;
constexpr uint32_t REG_IQA      = 0x090;

constexpr uint32_t GCMD_TE      = 1u << 31;
constexpr uint32_t GCMD_SRTP    = 1u << 30;
constexpr uint32_t GCMD_QIE     = 1u << 26;
constexpr uint32_t GSTS_TES     = 1u << 31;
constexpr uint32_t GSTS_RTPS    = 1u << 30;
constexpr uint32_t GSTS_QIES    = 1u << 26;
constexpr uint32_t GSTS_IRES    = 1u << 25;
constexpr uint32_t FSTS_PFO      = 1u << 0;
constexpr uint32_t FSTS_PPF      = 1u << 1;
constexpr uint32_t FECTL_IM      = 1u << 31;
constexpr uint32_t PMEN_EPM      = 1u << 31;
constexpr uint32_t PMEN_PRS      = 1u << 0;

constexpr uint64_t CCMD_ICC      = 1ull << 63;
constexpr uint64_t CCMD_GRAN_DEV = 3ull << 61;
constexpr uint64_t CCMD_GRAN_DOM = 2ull << 61;
constexpr uint64_t CCMD_DID(uint16_t d) { return (uint64_t)(d) & 0xffff; }
constexpr uint64_t CCMD_SID(uint16_t s) { return ((uint64_t)(s) & 0xffff) << 16; }

constexpr uint64_t TLB_IVT       = 1ull << 63;
constexpr uint64_t TLB_GRAN_DOM  = 2ull << 60;
constexpr uint64_t TLB_DID(uint16_t d) { return ((uint64_t)(d) & 0xffff) << 32; }
constexpr uint64_t TLB_RDRAIN    = 1ull << 49;
constexpr uint64_t TLB_WDRAIN    = 1ull << 48;

// context entry, legacy format
constexpr uint64_t CTX_PRESENT   = 1ull << 0;
constexpr uint64_t CTX_TT_SL     = 0ull;         // 00b: second-level translation
constexpr uint64_t CTX_TT_PT     = 2ull << 2;   // 10b: pass-through
constexpr uint64_t CTX_DID(uint16_t d) { return ((uint64_t)(d) & 0xffff) << 8; }

// second-level entries
constexpr uint64_t SL_RW         = 0x3;
constexpr uint64_t SL_PS         = 1ull << 7;

// queued invalidation descriptor types
constexpr uint64_t QI_CC_TYPE    = 1;
constexpr uint64_t QI_IOTLB_TYPE = 2;
constexpr uint64_t QI_IWD_TYPE   = 5;
constexpr uint64_t QI_GRAN_DEV   = 3ull << 4;
constexpr uint64_t QI_GRAN_DOM   = 2ull << 4;
constexpr uint64_t QI_CC_DID(uint16_t d) { return ((uint64_t)(d) & 0xffff) << 16; }
constexpr uint64_t QI_CC_SID(uint16_t s) { return ((uint64_t)(s) & 0xffff) << 32; }
constexpr uint64_t QI_TLB_DID(uint16_t d) { return ((uint64_t)(d) & 0xffff) << 16; }
constexpr uint64_t QI_TLB_RDR    = 1ull << 7;
constexpr uint64_t QI_TLB_WDR    = 1ull << 6;

constexpr int MAX_UNITS = 8;
constexpr int MAX_DOMAINS = 4;
// a vm passthrough domain maps all of a guest's ram in 4 KB pages (1 GB =
// 512 leaf tables), so the per-domain table list is bigger than a container
// needs; the identity build keeps its own on-stack list. (satoru)
constexpr int MAX_DOMAIN_TABLES = 2304;
constexpr int MAX_ID_TABLES = 256;
constexpr int MAX_DOMAIN_BDFS = 4;
constexpr int QI_ENTRIES = 256;

struct DmarUnit {
    uint64_t base;
    uint32_t segment;
    uint8_t  flags;          // bit 0: include_pci_all
    uint16_t length;
};

// ACPI DMAR reserved memory region (type 1). the limit is inclusive. (satoru)
struct DmarRmrr {
    uint64_t base;
    uint64_t end;
};

struct DmarScope {
    uint8_t type;
    uint8_t bus;
    uint8_t dev;
    uint8_t fn;
    uint8_t path_len;        // number of (dev,fn) pairs
    uint64_t drhd;
};

struct UnitCaps {
    bool     valid;
    uint64_t base;
    uint64_t cap;
    uint64_t ecap;
    uint32_t sagaw;
    uint32_t mgaw;
    uint32_t fault_off;
    uint32_t fault_n;
    bool     pt;
    bool     qi;
    uint32_t iro;
    bool     rdrain;
    bool     wdrain;
};

struct Domain {
    bool     used;
    uint64_t as;
    char     family[16];
    uint16_t did;
    int      aw;
    uint64_t root;
    uint64_t tables[MAX_DOMAIN_TABLES];
    int      ntables;
    uint32_t bdfs[MAX_DOMAIN_BDFS];
    int      nbdf;
    uint32_t mapped;
    uint32_t unmapped;
    bool     released;
    bool     blocked;
};

constexpr int MAX_TABLES_TS = 256;

struct TableSet {
    uint64_t* pages;
    int       n;
    int       max;
};

// ── state ───────────────────────────────────────────────────────────────────
bool g_enabled = true;
bool g_test;
bool g_native_test;
int  g_forced_aw;            // 0 = auto, 1 = 39-bit, 2 = 48-bit
bool g_parsed;
DmarUnit g_units[MAX_UNITS];
int g_nunits;
DmarScope g_scopes[64];
int g_nscopes;
DmarRmrr g_rmrr[8];
int g_nrmrr;

bool     g_on;               // translation enabled on g_unit
UnitCaps g_uc;
// persistent GCMD enable bits. GCMD is a write-only command register on real
// hardware; commands are built from this shadow, which starts from GSTS, so a
// later command write can never silently clear an earlier enable (F1).
uint32_t g_gcmd;
uint64_t g_root_phys;
uint64_t g_ctx_phys;
int      g_aw;
bool     g_qi_enabled;
uint64_t g_qi_phys;
uint64_t g_qi_status_phys;
int      g_qi_tail;
bool     g_identity;         // no pass-through: shared identity domain
uint64_t g_identity_root;

Domain g_domains[MAX_DOMAINS];
Spinlock g_lock;

uint64_t g_test_canary;
bool     g_canary_allocated;

// last container release, for the teardown self check. (satoru)
bool     g_release_valid;
bool     g_release_blocked;
uint32_t g_release_bdf;
uint32_t g_release_mapped;
uint32_t g_release_unmapped;
uint32_t g_release_tables;

// fault poller state
uint32_t g_fault_tokens;
uint64_t g_fault_refill_ms;
bool     g_overflow_logged;

// ── tiny output helpers ─────────────────────────────────────────────────────
void log(const char* s) { SerialLogger::Log(s); }
void log_dec(uint32_t v) { SerialLogger::LogDec((int)v); }
void log_hex(uint64_t v) { SerialLogger::LogHex64(v); }

void log_bdf(uint32_t bdf) {
    log_dec((bdf >> 16) & 0xFFu);
    log(":");
    log_dec((bdf >> 8) & 0xFFu);
    log(".");
    log_dec(bdf & 7u);
}

// ── memory helpers ──────────────────────────────────────────────────────────
uint64_t alloc_page() {
    uint64_t p = PMM::AllocContiguous(1);
    if (p) memset((void*)p, 0, 4096);
    return p;
}

void flush_range(uint64_t phys, uint64_t bytes) {
    // ecap.c == 0 means the remapping unit does not snoop the cpu caches, so
    // page-table writes must be flushed before the unit can see them. (satoru)
    uint64_t end = phys + bytes;
    for (uint64_t p = phys & ~63ull; p < end; p += 64)
        __asm__ __volatile__("clflush (%0)" : : "r"((void*)p) : "memory");
    __sync_synchronize();
}

int ts_add(TableSet* ts, uint64_t phys) {
    if (!ts) return 1;
    if (ts->n >= ts->max) return 0;
    ts->pages[ts->n++] = phys;
    return 1;
}

uint64_t entry_addr(uint64_t e) { return e & ~0xFFFull; }

// walk to the PT level and map one 4KB page. creates missing tables. (satoru)
bool map_4k(uint64_t root, int aw, uint64_t iova, uint64_t pa, TableSet* ts) {
    int levels = aw + 2;
    if (aw == 2 && (iova >> 48)) return false;
    if (aw == 1 && (iova >> 39)) return false;
    if (aw == 0 && (iova >> 30)) return false;
    uint64_t* tab = (uint64_t*)root;
    for (int lvl = levels; lvl >= 2; lvl--) {
        uint32_t shift = (uint32_t)(12 + 9 * (lvl - 1));
        uint32_t idx = (uint32_t)((iova >> shift) & 0x1FF);
        uint64_t e = tab[idx];
        if (!(e & 1)) {
            uint64_t nt = alloc_page();
            if (!nt) return false;
            if (!ts_add(ts, nt)) {
                PMM::FreeContiguous(nt, 1);
                return false;
            }
            tab[idx] = nt | SL_RW;
            e = tab[idx];
        } else if ((e & SL_PS) && lvl != 2) {
            return false;
        }
        tab = (uint64_t*)entry_addr(e);
    }
    uint32_t idx = (uint32_t)((iova >> 12) & 0x1FF);
    tab[idx] = (pa & ~0xFFFull) | SL_RW;
    return true;
}

// map one 2MB large page at the PD level. used only by the identity domain.
bool map_2m(uint64_t root, int aw, uint64_t iova, uint64_t pa, TableSet* ts) {
    int levels = aw + 2;
    uint64_t* tab = (uint64_t*)root;
    for (int lvl = levels; lvl >= 3; lvl--) {
        uint32_t shift = (uint32_t)(12 + 9 * (lvl - 1));
        uint32_t idx = (uint32_t)((iova >> shift) & 0x1FF);
        uint64_t e = tab[idx];
        if (!(e & 1)) {
            uint64_t nt = alloc_page();
            if (!nt) return false;
            if (!ts_add(ts, nt)) {
                PMM::FreeContiguous(nt, 1);
                return false;
            }
            tab[idx] = nt | SL_RW;
            e = tab[idx];
        } else if (e & SL_PS) {
            return false;
        }
        tab = (uint64_t*)entry_addr(e);
    }
    uint32_t idx = (uint32_t)((iova >> 21) & 0x1FF);
    tab[idx] = (pa & ~0x1FFFFFull) | SL_RW | SL_PS;
    return true;
}

bool unmap_4k(uint64_t root, int aw, uint64_t iova) {
    int levels = aw + 2;
    uint64_t* tab = (uint64_t*)root;
    for (int lvl = levels; lvl >= 2; lvl--) {
        uint32_t shift = (uint32_t)(12 + 9 * (lvl - 1));
        uint32_t idx = (uint32_t)((iova >> shift) & 0x1FF);
        uint64_t e = tab[idx];
        if (!(e & 1) || (e & SL_PS)) return false;
        tab = (uint64_t*)entry_addr(e);
    }
    uint32_t idx = (uint32_t)((iova >> 12) & 0x1FF);
    if (!(tab[idx] & 1)) return false;
    tab[idx] = 0;
    return true;
}

// ── MMIO access (identity map, uncached) ────────────────────────────────────
bool map_mmio(uint64_t base) {
    const uint64_t fl = PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_PWT | PTE_NX;
    return KernelVMM::MapPage(base & ~0xFFFull, base & ~0xFFFull, fl);
}

uint32_t rd32(uint64_t base, uint32_t off) {
    return *(volatile uint32_t*)(uintptr_t)(base + off);
}
void wr32(uint64_t base, uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(uintptr_t)(base + off) = v;
}
uint64_t rd64(uint64_t base, uint32_t off) {
    return *(volatile uint64_t*)(uintptr_t)(base + off);
}
void wr64(uint64_t base, uint32_t off, uint64_t v) {
    *(volatile uint64_t*)(uintptr_t)(base + off) = v;
}

bool wait_bit(uint64_t base, uint32_t reg, uint32_t bit, bool set, int ms) {
    uint64_t t0 = Scheduler::NowMs();
    while (Scheduler::NowMs() - t0 < (uint64_t)ms) {
        uint32_t v = rd32(base, reg);
        if (set ? (v & bit) : !(v & bit)) return true;
        __asm__ __volatile__("pause");
    }
    return false;
}

// gcmd is write-only on real hardware; build every command from the shadow
// of the persistent enable bits (gsts & 0x96ffffff at enable time). one-shot
// bits are passed in and never stored. (satoru)
void gcmd_write(uint32_t one_shot) {
    wr32(g_uc.base, REG_GCMD, (g_gcmd & 0x96FFFFFFu) | one_shot);
}

// ── ACPI: RSDP -> XSDT/RSDT -> DMAR ─────────────────────────────────────────
bool sig_eq(const uint8_t* p, const char* sig) {
    for (int i = 0; i < 4; i++)
        if (p[i] != (uint8_t)sig[i]) return false;
    return true;
}

uint64_t find_rsdp_legacy() {
    for (uint64_t a = 0xE0000; a < 0x100000; a += 16) {
        const uint8_t* p = (const uint8_t*)a;
        if (!(p[0] == 'R' && p[1] == 'S' && p[2] == 'D' && p[3] == ' ')) continue;
        uint8_t sum = 0;
        for (int i = 0; i < 20; i++) sum = (uint8_t)(sum + p[i]);
        if (sum == 0) return a;
    }
    return 0;
}

void parse_dmar_table(const uint8_t* t, uint32_t len) {
    // the DMAR table adds 12 bytes (host address width, flags, reserved)
    // after the 36-byte acpi header; structures start at 48. (satoru)
    uint32_t p = 48;
    while (p + 4 <= len) {
        const uint8_t* e = t + p;
        uint16_t type = (uint16_t)(e[0] | (e[1] << 8));
        uint16_t elen = (uint16_t)(e[2] | (e[3] << 8));
        if (elen < 4 || p + elen > len) break;
        if (type == 1 && elen >= 24 && g_nrmrr < 8) {
            uint32_t seg = (uint32_t)(e[6] | (e[7] << 8));
            if (seg == 0) {
                DmarRmrr& r = g_rmrr[g_nrmrr];
                r.base = 0;
                r.end = 0;
                for (int i = 0; i < 8; i++) {
                    r.base |= (uint64_t)e[8 + i] << (8 * i);
                    r.end |= (uint64_t)e[16 + i] << (8 * i);
                }
                g_nrmrr++;
            }
        }
        if (type == 0 && elen >= 16 && g_nunits < MAX_UNITS) {
            DmarUnit& u = g_units[g_nunits];
            u.flags = e[4];
            u.length = elen;
            u.segment = (uint32_t)(e[6] | (e[7] << 8));
            u.base = 0;
            for (int i = 0; i < 8; i++)
                u.base |= (uint64_t)e[8 + i] << (8 * i);
            // device scopes follow the 16-byte header
            uint32_t sp = 16;
            while (sp + 6 <= elen && g_nscopes < 64) {
                const uint8_t* s = e + sp;
                uint8_t slen = s[1];
                if (slen < 6 || sp + slen > elen) break;
                DmarScope& sc = g_scopes[g_nscopes];
                sc.type = s[0];
                sc.bus = s[5];
                sc.path_len = (uint8_t)((slen - 6) / 2);
                sc.dev = sc.path_len ? s[6] : 0;
                sc.fn  = sc.path_len ? s[7] : 0;
                sc.drhd = u.base;
                g_nscopes++;
                sp += slen;
            }
            g_nunits++;
        }
        p += elen;
    }
}

bool find_dmar() {
    if (g_parsed) return g_nunits > 0;
    g_parsed = true;
    uint64_t rsdp = BootAcpi::Rsdp();
    if (!rsdp) rsdp = find_rsdp_legacy();
    if (!rsdp) {
        log("[kld-iommu] no ACPI RSDP found\r\n");
        return false;
    }
    const uint8_t* r = (const uint8_t*)(uintptr_t)rsdp;
    uint8_t rev = r[15];
    uint64_t root = 0;
    bool xsdt = false;
    if (rev >= 2) {
        for (int i = 0; i < 8; i++) root |= (uint64_t)r[24 + i] << (8 * i);
        xsdt = root != 0;
    }
    if (!root) {
        for (int i = 0; i < 4; i++) root |= (uint64_t)r[16 + i] << (8 * i);
        xsdt = false;
    }
    if (!root) {
        log("[kld-iommu] no acpi root table\r\n");
        return false;
    }
    const uint8_t* rt = (const uint8_t*)(uintptr_t)root;
    uint32_t rtlen = (uint32_t)rt[4] | ((uint32_t)rt[5] << 8) |
                     ((uint32_t)rt[6] << 16) | ((uint32_t)rt[7] << 24);
    if (rtlen < 36 || rtlen > 0x100000) return false;
    uint32_t esz = xsdt ? 8 : 4;
    for (uint32_t off = 36; off + esz <= rtlen; off += esz) {
        uint64_t tp = 0;
        for (uint32_t i = 0; i < esz; i++) tp |= (uint64_t)rt[off + i] << (8 * i);
        if (!tp) continue;
        const uint8_t* t = (const uint8_t*)(uintptr_t)tp;
        if (!sig_eq(t, "DMAR")) continue;
        uint32_t len = (uint32_t)t[4] | ((uint32_t)t[5] << 8) |
                       ((uint32_t)t[6] << 16) | ((uint32_t)t[7] << 24);
        if (len < 36) return false;
        parse_dmar_table(t, len);
        if (g_nunits > 0)
            log("[kld-iommu] DMAR from ACPI: units and scopes parsed\r\n");
        return g_nunits > 0;
    }
    return false;
}

// does this unit own the device by an explicit scope? (satoru)
bool scope_matches(const DmarUnit& u, uint32_t bdf) {
    uint32_t bus = (bdf >> 16) & 0xFFu;
    uint8_t dev = (uint8_t)((bdf >> 8) & 0xFFu);
    uint8_t fn = (uint8_t)(bdf & 7u);
    for (int i = 0; i < g_nscopes; i++) {
        const DmarScope& s = g_scopes[i];
        if (s.drhd != u.base) continue;
        if (s.type == 1) {            // pci endpoint
            if (s.bus != bus) continue;
            if (s.path_len == 0) return true;
            if (s.path_len == 1 && s.dev == dev && s.fn == fn) return true;
        } else if (s.type == 2) {     // pci sub-hierarchy
            if (s.bus == bus) return true;
        }
    }
    return false;
}

int select_unit(uint32_t bdf) {
    uint32_t bus = (bdf >> 16) & 0xFFu;
    uint8_t dev = (uint8_t)((bdf >> 8) & 0xFFu);
    uint8_t fn = (uint8_t)(bdf & 7u);
    // 0000:00:02.0 belongs to the GPU agent: never take its remapping unit.
    if (bus == 0 && dev == 2 && fn == 0) {
        log("[kld-iommu] bdf 0:2.0 is reserved for the gpu agent, not isolated\r\n");
        return -1;
    }
    int all = -1;
    for (int i = 0; i < g_nunits; i++) {
        if (g_units[i].segment != 0) continue;
        if (scope_matches(g_units[i], bdf)) return i;
        if ((g_units[i].flags & 1) && all < 0) all = i;
    }
    return all;
}

// ── capability decode and unit print ────────────────────────────────────────
void read_caps(int i, UnitCaps* c) {
    memset(c, 0, sizeof(*c));
    c->base = g_units[i].base;
    c->cap = rd64(c->base, REG_CAP);
    c->ecap = rd64(c->base, REG_ECAP);
    c->sagaw = (uint32_t)((c->cap >> 8) & 0x1F);
    c->mgaw = (uint32_t)(((c->cap >> 16) & 0x3F) + 1);
    c->fault_off = (uint32_t)(((c->cap >> 24) & 0x3FF) * 16);
    c->fault_n = (uint32_t)(((c->cap >> 40) & 0xFF) + 1);
    c->pt = ((c->ecap >> 6) & 1) != 0;
    c->qi = ((c->ecap >> 1) & 1) != 0;
    c->iro = (uint32_t)(((c->ecap >> 8) & 0x3FF) * 16);
    c->rdrain = ((c->cap >> 55) & 1) != 0;
    c->wdrain = ((c->cap >> 54) & 1) != 0;
    c->valid = (c->cap != 0) && (c->cap != ~0ull);
}

void log_unit(int i) {
    UnitCaps c;
    read_caps(i, &c);
    log("[kld-iommu] unit ");
    log_dec((uint32_t)i);
    log(" base=0x"); log_hex(c.base);
    log(" seg="); log_dec(g_units[i].segment);
    log(" flags=0x"); log_hex(g_units[i].flags);
    log("\r\n");
    log("[kld-iommu]   ver=0x"); log_hex(rd32(c.base, REG_VER));
    log(" cap=0x"); log_hex(c.cap);
    log(" ecap=0x"); log_hex(c.ecap);
    log(" sagaw=0x"); log_hex(c.sagaw);
    log(" mgaw="); log_dec(c.mgaw);
    log(c.pt ? " pt" : " no-pt");
    log(c.qi ? " qi" : " no-qi");
    log("\r\n");
    log("[kld-iommu]   gsts=0x"); log_hex(rd32(c.base, REG_GSTS));
    log(" pmen=0x"); log_hex(rd32(c.base, REG_PMEN));
    log(" rtaddr=0x"); log_hex(rd64(c.base, REG_RTADDR));
    log("\r\n");
}

// ── invalidation ────────────────────────────────────────────────────────────
void qi_submit(uint64_t qw0, uint64_t qw1) {
    if (!g_qi_enabled) return;
    uint32_t head = (rd32(g_uc.base, REG_IQH) >> 4) & (QI_ENTRIES - 1);
    if (((g_qi_tail + 2) & (QI_ENTRIES - 1)) == head) {
        uint64_t t0 = Scheduler::NowMs();
        while (((g_qi_tail + 2) & (QI_ENTRIES - 1)) == head &&
               Scheduler::NowMs() - t0 < 100)
            __asm__ __volatile__("pause");
    }
    uint64_t* d = (uint64_t*)(uintptr_t)(g_qi_phys + (uint64_t)g_qi_tail * 16);
    d[0] = qw0;
    d[1] = qw1;
    d[2] = 0;
    d[3] = 0;
    flush_range((uint64_t)(uintptr_t)d, 16);
    g_qi_tail = (g_qi_tail + 1) & (QI_ENTRIES - 1);
    wr32(g_uc.base, REG_IQT, (uint32_t)(g_qi_tail * 16));
    __sync_synchronize();
}

void qi_wait() {
    if (!g_qi_enabled) return;
    uint64_t* st = (uint64_t*)(uintptr_t)g_qi_status_phys;
    *st = 0;
    flush_range(g_qi_status_phys, 64);
    // wait descriptor: status data 1, status write, no fence. (satoru)
    qi_submit(QI_IWD_TYPE | (1ull << 32) | (1ull << 5), g_qi_status_phys);
    uint64_t t0 = Scheduler::NowMs();
    while (*(volatile uint64_t*)(uintptr_t)g_qi_status_phys != 1) {
        if (Scheduler::NowMs() - t0 > 100) {
            log("[kld-iommu] qi wait timeout\r\n");
            break;
        }
        __asm__ __volatile__("pause");
    }
}

void flush_ctx_device(uint16_t did, uint16_t sid) {
    if (g_qi_enabled) {
        qi_submit(QI_CC_TYPE | QI_GRAN_DEV | QI_CC_DID(did) | QI_CC_SID(sid), 0);
        qi_wait();
        return;
    }
    uint64_t v = CCMD_ICC | CCMD_GRAN_DEV | CCMD_DID(did) | CCMD_SID(sid);
    wr64(g_uc.base, REG_CCMD, v);
    uint64_t t0 = Scheduler::NowMs();
    while ((rd64(g_uc.base, REG_CCMD) & CCMD_ICC) && Scheduler::NowMs() - t0 < 100)
        __asm__ __volatile__("pause");
}

void flush_iotlb_domain(uint16_t did) {
    if (g_qi_enabled) {
        uint64_t qw0 = QI_IOTLB_TYPE | QI_GRAN_DOM | QI_TLB_DID(did);
        if (g_uc.rdrain) qw0 |= QI_TLB_RDR;
        if (g_uc.wdrain) qw0 |= QI_TLB_WDR;
        qi_submit(qw0, 0);
        qi_wait();
        return;
    }
    uint64_t v = TLB_IVT | TLB_GRAN_DOM | TLB_DID(did);
    if (g_uc.rdrain) v |= TLB_RDRAIN;
    if (g_uc.wdrain) v |= TLB_WDRAIN;
    wr64(g_uc.base, g_uc.iro, 0);
    wr64(g_uc.base, g_uc.iro + 8, v);
    uint64_t t0 = Scheduler::NowMs();
    while ((rd64(g_uc.base, g_uc.iro + 8) & TLB_IVT) &&
           Scheduler::NowMs() - t0 < 100)
        __asm__ __volatile__("pause");
}

void flush_ctx_global() {
    if (g_qi_enabled) {
        qi_submit(QI_CC_TYPE | (1ull << 4), 0);   // global context-cache
        qi_wait();
        return;
    }
    uint64_t v = CCMD_ICC | (1ull << 61);
    wr64(g_uc.base, REG_CCMD, v);
    uint64_t t0 = Scheduler::NowMs();
    while ((rd64(g_uc.base, REG_CCMD) & CCMD_ICC) &&
           Scheduler::NowMs() - t0 < 100)
        __asm__ __volatile__("pause");
}

void flush_iotlb_global() {
    if (g_qi_enabled) {
        uint64_t qw0 = QI_IOTLB_TYPE | (1ull << 4);   // global iotlb
        if (g_uc.rdrain) qw0 |= QI_TLB_RDR;
        if (g_uc.wdrain) qw0 |= QI_TLB_WDR;
        qi_submit(qw0, 0);
        qi_wait();
        return;
    }
    uint64_t v = TLB_IVT | (1ull << 60);
    if (g_uc.rdrain) v |= TLB_RDRAIN;
    if (g_uc.wdrain) v |= TLB_WDRAIN;
    wr64(g_uc.base, g_uc.iro, 0);
    wr64(g_uc.base, g_uc.iro + 8, v);
    uint64_t t0 = Scheduler::NowMs();
    while ((rd64(g_uc.base, g_uc.iro + 8) & TLB_IVT) &&
           Scheduler::NowMs() - t0 < 100)
        __asm__ __volatile__("pause");
}

bool qi_enable() {
    g_qi_phys = alloc_page();
    g_qi_status_phys = alloc_page();
    if (!g_qi_phys || !g_qi_status_phys) return false;
    g_qi_tail = 0;
    wr32(g_uc.base, REG_IQT, 0);
    wr64(g_uc.base, REG_IQA, g_qi_phys);
    gcmd_write(GCMD_QIE);
    if (!wait_bit(g_uc.base, REG_GSTS, GSTS_QIES, true, 100)) {
        log("[kld-iommu] queued invalidation did not enable\r\n");
        return false;
    }
    g_gcmd |= GCMD_QIE;
    g_qi_enabled = true;
    return true;
}

// ── context tables ──────────────────────────────────────────────────────────
uint64_t* ctx_entry(uint32_t bdf) {
    uint32_t bus = (bdf >> 16) & 0xFFu;
    uint32_t devfn = ((bdf >> 8) & 0xFFu) * 8u + (bdf & 7u);
    if (bus > 255 || devfn > 255) return nullptr;
    return (uint64_t*)(uintptr_t)(g_ctx_phys + (uint64_t)bus * 4096 +
                                  (uint64_t)devfn * 16);
}

void set_context(uint32_t bdf, uint64_t lo, uint64_t hi) {
    uint64_t* ce = ctx_entry(bdf);
    if (!ce) return;
    ce[0] = 0;                 // drop present while the fields change (satoru)
    __sync_synchronize();
    ce[1] = hi;
    flush_range((uint64_t)(uintptr_t)ce, 16);
    ce[0] = lo;
    flush_range((uint64_t)(uintptr_t)ce, 16);
    __sync_synchronize();
}

uint64_t identity_aw_root(int aw, TableSet* ts) {
    uint64_t root = alloc_page();
    if (!root) return 0;
    if (!ts_add(ts, root)) return 0;
    uint64_t top = PMM::GetPhysTop();
    uint64_t limit = top > 0x100000000ull ? top : 0x100000000ull;
    if (aw == 0) limit = 0x40000000ull;      // 30-bit window
    limit = (limit + 0x1FFFFFull) & ~0x1FFFFFull;
    for (uint64_t p = 0; p < limit; p += 0x200000ull) {
        if (!map_2m(root, aw, p, p, ts)) {
            log("[kld-iommu] identity 2MB map failed at 0x");
            log_hex(p);
            log("\r\n");
            return 0;
        }
    }
    // RMRR ranges above the flat mapping (RAM + low MMIO) must stay reachable
    // for the devices firmware reserved them for. (satoru)
    for (int i = 0; i < g_nrmrr; i++) {
        uint64_t b = g_rmrr[i].base & ~0x1FFFFFull;
        uint64_t e = (g_rmrr[i].end + 0x1FFFFFull) & ~0x1FFFFFull;
        if (e <= limit) continue;
        if (b < limit) b = limit;
        for (uint64_t p = b; p < e; p += 0x200000ull) {
            if (!map_2m(root, aw, p, p, ts)) {
                log("[kld-iommu] identity RMRR map failed at 0x");
                log_hex(p);
                log("\r\n");
                return 0;
            }
        }
    }
    if (g_nrmrr)
        log("[kld-iommu] identity domain includes the DMAR RMRR ranges\r\n");
    return root;
}

bool build_context_tables() {
    g_root_phys = alloc_page();
    uint64_t ctx = PMM::AllocContiguous(256);
    if (!g_root_phys || !ctx) return false;
    g_ctx_phys = ctx;
    memset((void*)ctx, 0, 256 * 4096);
    // every root entry is 128 bits: lo = present + context table address,
    // hi = 0. indexing it as one u64 per bus corrupts the high halves.
    // (satoru)
    uint64_t* root = (uint64_t*)g_root_phys;
    for (int bus = 0; bus < 256; bus++) {
        root[bus * 2 + 0] = (ctx + (uint64_t)bus * 4096) | 1;
        root[bus * 2 + 1] = 0;
    }
    flush_range(g_root_phys, 4096);
    flush_range(ctx, 256 * 4096);

    if (!g_identity) {
        for (int bus = 0; bus < 256; bus++) {
            uint64_t* tab = (uint64_t*)(uintptr_t)(ctx + (uint64_t)bus * 4096);
            for (int i = 0; i < 256; i++) {
                tab[i * 2 + 0] = CTX_PRESENT | CTX_TT_PT;
                // AW still has to be a supported width and DID must not be 0
                // in caching mode; 1 is the shared non-container domain.
                // (satoru)
                tab[i * 2 + 1] = (uint64_t)g_aw | CTX_DID(1);
            }
        }
        return true;
    }

    uint64_t id_pages[MAX_ID_TABLES];
    TableSet ts = { id_pages, 0, MAX_ID_TABLES };
    g_identity_root = identity_aw_root(g_aw, &ts);
    if (!g_identity_root) {
        // the identity tables are kept while translation is on. (satoru)
        for (int i = 0; i < ts.n; i++) PMM::FreeContiguous(ts.pages[i], 1);
        return false;
    }
    for (int bus = 0; bus < 256; bus++) {
        uint64_t* tab = (uint64_t*)(uintptr_t)(ctx + (uint64_t)bus * 4096);
        for (int i = 0; i < 256; i++) {
            tab[i * 2 + 0] = CTX_PRESENT | CTX_TT_SL | g_identity_root;
            tab[i * 2 + 1] = (uint64_t)g_aw | CTX_DID(1);
        }
    }
    flush_range(ctx, 256 * 4096);
    return true;
}

// ── domains ─────────────────────────────────────────────────────────────────
Domain* find_domain(uint64_t as) {
    for (int i = 0; i < MAX_DOMAINS; i++)
        if (g_domains[i].used && g_domains[i].as == as) return &g_domains[i];
    return nullptr;
}

Domain* find_domain_bdf(uint32_t bdf) {
    for (int i = 0; i < MAX_DOMAINS; i++) {
        if (!g_domains[i].used) continue;
        for (int b = 0; b < g_domains[i].nbdf; b++)
            if (g_domains[i].bdfs[b] == bdf) return &g_domains[i];
    }
    return nullptr;
}

Domain* alloc_domain(uint64_t as, const char* family) {
    for (int i = 0; i < MAX_DOMAINS; i++) {
        if (g_domains[i].used) continue;
        Domain& d = g_domains[i];
        memset(&d, 0, sizeof(d));
        d.used = true;
        d.as = as;
        d.did = (uint16_t)(2 + i);
        d.aw = g_aw;
        int k = 0;
        if (family) {
            while (family[k] && k < 15) { d.family[k] = family[k]; k++; }
        }
        d.family[k] = 0;
        d.root = alloc_page();
        if (!d.root) {
            d.used = false;
            return nullptr;
        }
        d.tables[d.ntables++] = d.root;
        return &d;
    }
    return nullptr;
}

void free_domain_tables(Domain* d) {
    for (int i = 0; i < d->ntables; i++)
        PMM::FreeContiguous(d->tables[i], 1);
    d->tables[0] = 0;
    d->ntables = 0;
}

// ── fault records ───────────────────────────────────────────────────────────
const char* fault_reason_text(uint32_t r) {
    static const char* legacy[] = {
        "software", "root entry not present", "context entry not present",
        "invalid context entry", "access beyond mgaw",
        "pte write access not set", "pte read access not set",
        "next page table ptr invalid", "root table address invalid",
        "context table ptr invalid", "reserved bits in rtp",
        "reserved bits in ctp", "reserved bits in pte", "blocked by pce",
    };
    if (r < sizeof(legacy) / sizeof(legacy[0])) return legacy[r];
    return "other";
}

void drain_faults() {
    if (!g_on) return;
    uint32_t sts = rd32(g_uc.base, REG_FSTS);
    if (!sts) return;
    if (sts & FSTS_PFO) {
        if (!g_overflow_logged) {
            g_overflow_logged = true;
            log("[kld-iommu] fault record overflow\r\n");
        }
    }
    if (!(sts & FSTS_PPF)) {
        wr32(g_uc.base, REG_FSTS, sts);
        return;
    }
    uint32_t idx = (sts >> 8) & 0xFF;
    if (idx >= g_uc.fault_n) idx = 0;
    uint32_t hi = rd32(g_uc.base, (uint32_t)(g_uc.fault_off + idx * 16 + 12));
    if (!(hi & 0x80000000u)) {
        wr32(g_uc.base, REG_FSTS, sts);
        return;
    }
    uint32_t reason = hi & 0xFFu;
    uint32_t lo = rd32(g_uc.base, (uint32_t)(g_uc.fault_off + idx * 16 + 8));
    uint16_t sid = (uint16_t)(lo & 0xFFFFu);
    uint64_t addr = rd64(g_uc.base, (uint32_t)(g_uc.fault_off + idx * 16));
    addr &= ~0xFFFull;
    uint32_t bdf = ((uint32_t)(sid >> 8) << 16) |
                   ((uint32_t)((sid >> 3) & 0x1F) << 8) | (sid & 7u);
    Domain* d = find_domain_bdf(bdf);

    uint64_t now = Scheduler::NowMs();
    if (now - g_fault_refill_ms >= 200) {
        g_fault_refill_ms = now;
        if (g_fault_tokens < 5) g_fault_tokens = 5;
    }
    if (g_fault_tokens) {
        g_fault_tokens--;
        log("[kld-iommu] fault bdf=");
        log_bdf(bdf);
        log(" addr=0x"); log_hex(addr);
        log(" reason="); log_dec(reason);
        log(" (");
        log(fault_reason_text(reason));
        log(") (container ");
        log(d ? d->family : "?");
        log(")\r\n");
    }

    wr32(g_uc.base, (uint32_t)(g_uc.fault_off + idx * 16 + 12), 0x80000000u);
    wr32(g_uc.base, REG_FSTS, sts);
}

}  // namespace

void NativeTestEntry();

// ── public ──────────────────────────────────────────────────────────────────
void SetEnabled(bool on) { g_enabled = on; }
bool Enabled() { return g_enabled; }
void SetTestMode(bool on) { g_test = on; }
bool TestMode() { return g_test; }
void SetNativeTest(bool on) { g_native_test = on; }
void SetForcedAw(int bits) {
    g_forced_aw = (bits == 39) ? 1 : (bits == 48) ? 2 : 0;
}
bool IsOn() { return g_on; }

bool EnsureOn(uint32_t bdf)
{
    if (g_on) return true;
    if (!g_enabled) return false;
    if (!find_dmar()) {
        log("[kld-iommu] no VT-d DMAR table, translation stays off\r\n");
        return false;
    }
    for (int i = 0; i < g_nunits; i++)
        if (!map_mmio(g_units[i].base)) {
            log("[kld-iommu] cannot map unit mmio, translation stays off\r\n");
            return false;
        }
    for (int i = 0; i < g_nunits; i++)
        log_unit(i);

    int ui = select_unit(bdf);
    if (ui < 0) {
        log("[kld-iommu] no remapping unit covers this device, translation stays off\r\n");
        return false;
    }
    UnitCaps c;
    read_caps(ui, &c);
    if (!c.valid) {
        log("[kld-iommu] unit caps unreadable, translation stays off\r\n");
        return false;
    }

    uint32_t gsts = rd32(c.base, REG_GSTS);
    if ((gsts & GSTS_TES) || (gsts & GSTS_IRES)) {
        log("[kld-iommu] unit at 0x");
        log_hex(c.base);
        log(" already enabled by firmware (TES=");
        log_dec((gsts & GSTS_TES) ? 1 : 0);
        log(",IRES=");
        log_dec((gsts & GSTS_IRES) ? 1 : 0);
        log("): container DMA is NOT isolated\r\n");
        return false;
    }

    int aw;
    if (g_forced_aw == 1) {
        if (!(c.sagaw & 0x2)) {
            log("[kld-iommu] forced 39-bit but SAGAW lacks it\r\n");
            return false;
        }
        aw = 1;
    } else if (g_forced_aw == 2) {
        if (!(c.sagaw & 0x4)) {
            log("[kld-iommu] forced 48-bit but SAGAW lacks it\r\n");
            return false;
        }
        aw = 2;
    } else if (c.sagaw & 0x4) {
        aw = 2;
    } else if (c.sagaw & 0x2) {
        aw = 1;
    } else if (c.sagaw & 0x1) {
        aw = 0;
    } else {
        log("[kld-iommu] no usable SAGAW, translation stays off\r\n");
        return false;
    }

    g_lock.Lock();
    g_uc = c;
    g_aw = aw;
    g_identity = !c.pt;
    if (!build_context_tables()) {
        g_lock.Unlock();
        log("[kld-iommu] page table build failed, translation stays off\r\n");
        return false;
    }
    // firmware protected memory regions: drop EPM and wait for PRS. (satoru)
    uint32_t pmen = rd32(c.base, REG_PMEN);
    if (pmen & PMEN_EPM) {
        pmen &= ~PMEN_EPM;
        wr32(c.base, REG_PMEN, pmen);
        uint64_t t0 = Scheduler::NowMs();
        while ((rd32(c.base, REG_PMEN) & PMEN_PRS) &&
               Scheduler::NowMs() - t0 < 100)
            __asm__ __volatile__("pause");
        log("[kld-iommu] cleared firmware PMEN.EPM on the selected unit\r\n");
    }

    // F1: never build a command from a GCMD read. the shadow carries the
    // persistent enables; GSTS is the only status source. (satoru)
    g_gcmd = rd32(c.base, REG_GSTS) & 0x96FFFFFFu;
    wr64(c.base, REG_RTADDR, g_root_phys);
    gcmd_write(GCMD_SRTP);
    if (!wait_bit(c.base, REG_GSTS, GSTS_RTPS, true, 100)) {
        g_lock.Unlock();
        log("[kld-iommu] SRTP did not take, translation stays off\r\n");
        return false;
    }

    if (c.qi) {
        if (!qi_enable()) {
            g_lock.Unlock();
            log("[kld-iommu] no invalidation engine, translation stays off\r\n");
            return false;
        }
    }

    // F2: hardware may still hold the firmware's context/IOTLB entries from
    // its own tables. invalidate both globally after SRTP, before TE. (satoru)
    flush_ctx_global();
    flush_iotlb_global();

    wr32(c.base, REG_FSTS, 0xFFFFFFFFu);
    uint32_t fectl = rd32(c.base, REG_FECTL);
    fectl |= FECTL_IM;
    wr32(c.base, REG_FECTL, fectl);
    g_fault_tokens = 5;
    g_fault_refill_ms = 0;

    gcmd_write(GCMD_TE);
    if (!wait_bit(c.base, REG_GSTS, GSTS_TES, true, 1000)) {
        gcmd_write(0);
        g_lock.Unlock();
        log("[kld-iommu] TE did not take, translation stays off\r\n");
        return false;
    }
    g_gcmd |= GCMD_TE;

    g_on = true;
    g_lock.Unlock();

    int other = KldBroker::ScanDeviceCount();
    // the scan counts every function on the machine; this device is one of
    // them. (satoru)
    if (other > 0) other--;
    log("[kld-iommu] vt-d on: ");
    log_dec((uint32_t)(other < 0 ? 0 : other));
    log(g_identity ? " other devices identity, " : " other devices pass-through, ");
    log_dec((uint32_t)(c.qi ? 1 : 0));
    log(" qi, aw=");
    log_dec((uint32_t)(30 + aw * 9));
    log("\r\n");
    Scheduler::SpawnKernelProcess("kld-iommu-fault", FaultPollEntry,
                                  PRIO_LOW, 64, 16 * 1024);
    if (g_native_test)
        Scheduler::SpawnKernelProcess("kld-iommu-native", NativeTestEntry,
                                      PRIO_LOW, 64, 16 * 1024);
    return true;
}

bool Attach(uint64_t as, const char* family, uint32_t bdf)
{
    if (!g_on) return false;
    g_lock.Lock();
    Domain* d = find_domain(as);
    if (!d) {
        d = alloc_domain(as, family);
        if (!d) {
            g_lock.Unlock();
            log("[kld-iommu] no domain slot for the container\r\n");
            return false;
        }
    }
    if (d->nbdf >= MAX_DOMAIN_BDFS) {
        g_lock.Unlock();
        return false;
    }
    d->bdfs[d->nbdf++] = bdf;
    uint64_t lo = CTX_PRESENT | CTX_TT_SL | d->root;
    uint64_t hi = (uint64_t)d->aw | CTX_DID(d->did);
    uint16_t sid = (uint16_t)(((bdf >> 16) & 0xFFu) << 8) |
                   (uint16_t)((((bdf >> 8) & 0xFFu) << 3) | (bdf & 7u));
    set_context(bdf, lo, hi);
    flush_ctx_device(d->did, sid);
    flush_iotlb_domain(d->did);
    g_lock.Unlock();

    log("[kld-iommu] domain ");
    log_dec(d->did);
    log(" for ");
    log(family && family[0] ? family : "?");
    log(" bdf ");
    log_bdf(bdf);
    log(" aw=");
    log_dec((uint32_t)(30 + d->aw * 9));
    log("\r\n");
    return true;
}

bool MapDma(uint64_t as, uint64_t phys, uint32_t pages)
{
    if (!g_on) return true;
    g_lock.Lock();
    Domain* d = find_domain(as);
    if (!d) {
        g_lock.Unlock();
        return true;   // not a device we attached: nothing to isolate
    }
    TableSet ts = { d->tables, d->ntables, MAX_DOMAIN_TABLES };
    for (uint32_t i = 0; i < pages; i++) {
        uint64_t pa = phys + (uint64_t)i * 4096;
        if (!map_4k(d->root, d->aw, pa, pa, &ts)) {
            d->ntables = ts.n;
            for (uint32_t j = 0; j < i; j++)
                if (unmap_4k(d->root, d->aw, phys + (uint64_t)j * 4096))
                    d->mapped--;
            flush_iotlb_domain(d->did);
            g_lock.Unlock();
            log("[kld-iommu] dma map failed for the container domain\r\n");
            return false;
        }
        d->mapped++;
    }
    d->ntables = ts.n;
    flush_range(d->root, 4096);
    flush_iotlb_domain(d->did);
    g_lock.Unlock();
    return true;
}

bool MapIova(uint64_t as, uint64_t iova, uint64_t phys, uint32_t pages)
{
    if (!g_on) return false;
    g_lock.Lock();
    Domain* d = find_domain(as);
    if (!d) {
        g_lock.Unlock();
        return false;
    }
    TableSet ts = { d->tables, d->ntables, MAX_DOMAIN_TABLES };
    bool ok = true;
    for (uint32_t i = 0; i < pages; i++) {
        if (!map_4k(d->root, d->aw, iova + (uint64_t)i * 4096,
                    phys + (uint64_t)i * 4096, &ts)) {
            ok = false;
            break;
        }
        d->mapped++;
    }
    d->ntables = ts.n;
    g_lock.Unlock();
    if (!ok) log("[kld-iommu] iova map failed (table list full?)\r\n");
    return ok;
}

void FlushDomain(uint64_t as)
{
    if (!g_on) return;
    g_lock.Lock();
    Domain* d = find_domain(as);
    if (d) {
        // map_4k leaves the leaf writes in the cache; a non-snooping unit
        // must see every table page (satoru)
        for (int i = 0; i < d->ntables; i++) flush_range(d->tables[i], 4096);
        flush_iotlb_domain(d->did);
    }
    g_lock.Unlock();
}

void UnmapDma(uint64_t as, uint64_t phys, uint32_t pages)
{
    if (!g_on) return;
    g_lock.Lock();
    Domain* d = find_domain(as);
    if (!d) {
        g_lock.Unlock();
        return;
    }
    for (uint32_t i = 0; i < pages; i++) {
        uint64_t pa = phys + (uint64_t)i * 4096;
        if (unmap_4k(d->root, d->aw, pa)) {
            d->mapped--;
            d->unmapped++;
        }
    }
    flush_iotlb_domain(d->did);
    g_lock.Unlock();
}

void ReleaseContainer(uint64_t as)
{
    if (!g_on) return;
    g_lock.Lock();
    Domain* d = find_domain(as);
    if (!d) {
        g_lock.Unlock();
        return;
    }
    for (int b = 0; b < d->nbdf; b++) {
        uint32_t bdf = d->bdfs[b];
        uint64_t* ce = ctx_entry(bdf);
        if (ce) {
            ce[0] &= ~CTX_PRESENT;
            __sync_synchronize();
            flush_range((uint64_t)(uintptr_t)ce, 8);
        }
        uint16_t sid = (uint16_t)(((bdf >> 16) & 0xFFu) << 8) |
                       (uint16_t)((((bdf >> 8) & 0xFFu) << 3) | (bdf & 7u));
        flush_ctx_device(d->did, sid);
        flush_iotlb_domain(d->did);
    }
    uint32_t mapped = d->mapped;
    uint32_t unmapped = d->unmapped;
    int tables = d->ntables;
    uint16_t did = d->did;
    d->blocked = true;
    d->released = true;
    g_release_valid = true;
    g_release_blocked = true;
    g_release_bdf = d->bdfs[0];
    g_release_mapped = mapped;
    g_release_unmapped = unmapped;
    g_release_tables = (uint32_t)tables;
    free_domain_tables(d);
    d->used = false;   // the counts above stay in the log line
    g_lock.Unlock();

    log("[kld-iommu] domain ");
    log_dec(did);
    log(" released: device blocked, mapped ");
    log_dec(mapped);
    log(" frames, unmapped ");
    log_dec(unmapped);
    log(", page tables freed ");
    log_dec((uint32_t)tables);
    log("\r\n");
}

bool TestCanary(uint64_t* phys)
{
    if (!g_test) return false;
    if (!g_canary_allocated) {
        uint64_t p = PMM::AllocContiguous(1);
        if (!p) return false;
        memset((void*)p, 0xA5, 4096);
        g_test_canary = p;
        g_canary_allocated = true;
        log("[kld-iommu] test canary at 0x");
        log_hex(p);
        log("\r\n");
    }
    if (phys) *phys = g_test_canary;
    return true;
}

// one native DMA after translation is on: a sector read through the kernel's
// own NVMe driver. the pass-through context for this device must carry a real
// AGAW and a nonzero domain id. (satoru)
void NativeTestEntry()
{
    Scheduler::SleepMs(10000);
    uint64_t buf = PMM::AllocContiguous(8);
    if (!buf || !NVMe::IsDetected()) {
        log("[kld-iommu] native NVMe read SKIP\r\n");
        for (;;) Scheduler::SleepMs(60000);
    }
    memset((void*)buf, 0, 8 * 4096);
    bool ok = NVMe::Read(0, 8, (void*)buf);
    log("[kld-iommu] native NVMe read sector 0 ");
    log(ok ? "PASS" : "FAIL");
    log("\r\n");
    PMM::FreeContiguous(buf, 8);
    for (;;) Scheduler::SleepMs(60000);
}

void TestCheckEntry()
{
    Scheduler::SleepMs(35000);
    if (!g_canary_allocated) {
        log("[kld-iommu] test canary was never allocated\r\n");
        for (;;) Scheduler::SleepMs(60000);
    }
    const uint8_t* p = (const uint8_t*)(uintptr_t)g_test_canary;
    uint32_t bad = 0;
    for (int i = 0; i < 4096; i++)
        if (p[i] != 0xA5) bad++;
    if (bad == 0) {
        log("[kld-iommu] canary at 0x"); log_hex(g_test_canary);
        log(" INTACT (device write blocked) PASS\r\n");
    } else {
        log("[kld-iommu] canary at 0x"); log_hex(g_test_canary);
        log(" OVERWRITTEN ("); log_dec(bad);
        log(" bytes changed, device reached kernel memory) FAIL\r\n");
    }
    if (g_release_valid) {
        uint64_t* ce = ctx_entry(g_release_bdf);
        bool present = ce && ((ce[0] & CTX_PRESENT) != 0);
        log("[kld-iommu] teardown bdf=");
        log_bdf(g_release_bdf);
        log(" context ");
        log(present ? "STILL PRESENT" : "blocked");
        log(", mapped "); log_dec(g_release_mapped);
        log(" frames, unmapped "); log_dec(g_release_unmapped);
        log(", page tables freed "); log_dec(g_release_tables);
        log((!present && g_release_blocked) ? " PASS\r\n" : " FAIL\r\n");
    } else {
        log("[kld-iommu] teardown check: no container release seen\r\n");
    }
    for (;;) Scheduler::SleepMs(60000);
}

void FaultPollEntry()
{
    for (;;) {
        Scheduler::SleepMs(100);
        if (g_on) drain_faults();
    }
}

}  // namespace KldIommu

// end (satoru)
