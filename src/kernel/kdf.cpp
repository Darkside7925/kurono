//  kurono os: kdf, the kernel driver framework implementation.
//
//  see kdf.h for the design. the load-bearing pieces here:
//
//   * a per-driver virtual window (KDF_VA_BASE + id*stride) that kdf hands out
//     guard-fenced regions from. the payload is backed by real pmm frames mapped
//     into that window; one unmapped page sits before and after every region, so
//     an out-of-bounds dereference walks into an unmapped page -> #pf. the
//     physical frames are ALSO identity-mapped (pmm hands them out that way) so
//     the device's dma sees a normal contiguous physical buffer; kdf programming
//     hands the device PhysOf(va), not the guard-va. (satoru)
//
//   * a crash sandbox built on __builtin_setjmp/__builtin_longjmp (freestanding,
//     no libc). RunGuarded() saves a jmp context on a per-cpu stack of armed
//     frames; the #pf hook (HandleGuardFault, called from hal.cpp) longjmps back
//     to the innermost armed frame after quarantining the faulting region and
//     reporting the crash. mirrors how userspace.cpp uses UserspaceEnter/Resume
//     to unwind a faulting ring-3 process without panicking. (satoru)

#include "kdf.h"
#include "vmm.h"
#include "pmm.h"
#include "../drivers/serial.h"
#include "../hal/hal.h"
#include "../proc/smp.h"
#include "../fs/kvfs.h"
#include "../system/kpaths.h"

namespace KDF {

namespace {

// ── freestanding helpers (satoru) ────────────────────────────────────────────
int kd_len(const char* s) { int n = 0; while (s && s[n]) n++; return n; }
bool kd_eq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == 0 && *b == 0;
}
void kd_cpy(char* d, const char* s, int mx) {
    int i = 0; while (s && s[i] && i < mx - 1) { d[i] = s[i]; i++; } d[i] = 0;
}
int kd_cat(char* o, int p, int mx, const char* s) {
    while (s && *s && p < mx - 1) o[p++] = *s++;
    if (p < mx) o[p] = 0;
    return p;
}
int kd_cat_u(char* o, int p, int mx, uint64_t v) {
    char t[24]; int n = 0;
    if (v == 0) t[n++] = '0';
    else while (v > 0 && n < 24) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n > 0 && p < mx - 1) o[p++] = t[--n];
    if (p < mx) o[p] = 0;
    return p;
}
int kd_cat_hex(char* o, int p, int mx, uint64_t v) {
    const char* hx = "0123456789ABCDEF";
    p = kd_cat(o, p, mx, "0x");
    for (int i = 60; i >= 0; i -= 4)
        if (p < mx - 1) o[p++] = hx[(v >> i) & 0xF];
    if (p < mx) o[p] = 0;
    return p;
}

// ── driver table (satoru) ────────────────────────────────────────────────────
struct KDFDriver {
    bool       in_use;
    char       name[KDF_NAME_LEN];
    KDFInitFn  init;
    KDFState   state;
    uint64_t   va_base;       // this driver's window base (satoru)
    uint64_t   va_cursor;     // next free va within the window (satoru)
    KDFRegion  regions[KDF_MAX_REGIONS];
    int        region_count;
    uint32_t   crash_count;
    uint64_t   last_fault_addr;
    uint64_t   last_fault_rip;
};

KDFDriver g_drv[KDF_MAX_DRIVERS];
int       g_drv_count = 0;
bool      g_inited = false;
bool      g_log_to_fs = false;   // set true once kvfs is confirmed up (satoru)

// kinit's crash-notify hook (registered via SetCrashNotifier so kdf.cpp does not
// hard-depend on kinit, avoiding an init-order/link cycle). (satoru)
void (*g_crash_notify)(const char* driver, const char* reason) = nullptr;

// ── crash sandbox: per-cpu stack of armed setjmp frames (satoru) ─────────────
// __builtin_setjmp needs a buffer of at least 5 words on x86_64. each armed
// RunGuarded() pushes one; HandleGuardFault longjmps to the top of this cpu's
// stack. a small fixed depth covers reasonable driver-call nesting. (satoru)
constexpr int KDF_JMP_WORDS  = 8;     // generous; gcc x86_64 uses 5 (satoru)
constexpr int KDF_ARM_DEPTH  = 8;     // max nested RunGuarded per cpu (satoru)

struct ArmedFrame {
    void*    jb[KDF_JMP_WORDS];
    int      driver_id;
    volatile bool faulted;
};

struct PerCpuArm {
    ArmedFrame stack[KDF_ARM_DEPTH];
    int        depth;
};
PerCpuArm g_arm[SMP_MAX_CPUS];

PerCpuArm& this_cpu_arm() {
    uint32_t c = SMP::CpuIndex();
    if (c >= SMP_MAX_CPUS) c = 0;
    return g_arm[c];
}

bool is_kdf_va(uint64_t va) {
    return va >= KDF_VA_BASE &&
           va <  KDF_VA_BASE + (uint64_t)KDF_MAX_DRIVERS * KDF_DRIVER_VA_STRIDE;
}

// map a kdf payload range into the higher-half window. frames come from pmm
// (identity-mapped already); we ADD the higher-half alias so the driver sees a
// guard-fenced va while the device still dma's the identity phys. writable,
// no-user, present. (satoru)
bool map_payload(uint64_t va, uint64_t phys, uint64_t pages) {
    for (uint64_t i = 0; i < pages; i++) {
        if (!KernelVMM::MapPage(va + i * KDF_PAGE_SIZE,
                                phys + i * KDF_PAGE_SIZE,
                                PTE_PRESENT | PTE_WRITABLE | PTE_NX))
            return false;
    }
    return true;
}

// allocate `pages` of guard-fenced va in driver d: [guard][payload..][guard].
// returns the user_va (first payload byte) and fills *out_region_va / *out_total.
// the payload is backed by `phys` (contiguous). (satoru)
uint64_t carve_region(KDFDriver* d, uint64_t phys, uint64_t pages,
                      uint64_t* out_region_va, uint64_t* out_total_pages) {
    uint64_t total = pages + 2;                 // lead + trail guard (satoru)
    uint64_t region_va = d->va_cursor;
    // keep regions inside the driver's 1 GiB slice (satoru)
    if (region_va + total * KDF_PAGE_SIZE >
        d->va_base + KDF_DRIVER_VA_STRIDE) return 0;
    uint64_t user_va = region_va + KDF_PAGE_SIZE;   // skip the lead guard (satoru)
    if (!map_payload(user_va, phys, pages)) return 0;
    // advance cursor past the trailing guard, plus a one-page gap so adjacent
    // regions never share a guard slot. (satoru)
    d->va_cursor = region_va + (total + 1) * KDF_PAGE_SIZE;
    *out_region_va   = region_va;
    *out_total_pages = total;
    return user_va;
}

// find which driver + region a kdf va belongs to. matches the whole slice
// (guards included) so a guard-page fault resolves to its region. (satoru)
KDFRegion* region_for_va(uint64_t va, KDFDriver** out_drv) {
    for (int i = 0; i < g_drv_count; i++) {
        KDFDriver* d = &g_drv[i];
        if (!d->in_use) continue;
        if (va < d->va_base || va >= d->va_base + KDF_DRIVER_VA_STRIDE) continue;
        for (int r = 0; r < d->region_count; r++) {
            KDFRegion* rg = &d->regions[r];
            if (rg->kind == KDF_REGION_NONE) continue;
            uint64_t lo = rg->region_va;
            uint64_t hi = rg->region_va + rg->total_pages * KDF_PAGE_SIZE;
            if (va >= lo && va < hi) { if (out_drv) *out_drv = d; return rg; }
        }
        // inside the driver's window but not in a known region: still kdf, blame
        // this driver. (satoru)
        if (out_drv) *out_drv = d;
        return nullptr;
    }
    return nullptr;
}

// the driver kdf is CURRENTLY allocating resources for (set during Start /
// RunGuarded). resource apis attribute their regions to it. (satoru)
int g_active_driver = -1;

}  // namespace

// ── public: install kinit's crash notifier (satoru) ──────────────────────────
void SetCrashNotifier(void (*fn)(const char*, const char*)) { g_crash_notify = fn; }

// enable mirroring driver-log lines into /kurono/var/log/drivers.log. called
// once kvfs is confirmed up (kernel_main, right after KVFS::Init). before this,
// LogDriver only hits serial. (satoru)
void EnableFileLog() { g_log_to_fs = true; }

// ── lifecycle ────────────────────────────────────────────────────────────────
void Init() {
    if (g_inited) return;
    for (int i = 0; i < KDF_MAX_DRIVERS; i++) {
        for (int b = 0; b < (int)sizeof(KDFDriver); b++) ((char*)&g_drv[i])[b] = 0;
    }
    for (int c = 0; c < SMP_MAX_CPUS; c++) g_arm[c].depth = 0;
    g_drv_count = 0;
    g_active_driver = -1;

    // reserve the kdf higher-half pml4 sub-tree NOW (before any user address
    // space is cloned) by touching one page in each driver slice's range with a
    // map+unmap. this forces KernelVMM to allocate the pdpt/pd for the kdf pml4
    // entry in the kernel root, so the entry exists and is copied into every
    // user address space created later (CreateAddressSpace copies all 512 pml4
    // entries). without this, a driver registered after a user process spawned
    // could map into a pml4 slot the user space never saw  -  harmless for kernel-
    // cr3 driver code, but this keeps the window globally coherent. (satoru)
    uint64_t probe = KDF_VA_BASE;
    uint64_t probe_phys = PMM::AllocFrame();
    if (probe_phys) {
        KernelVMM::MapPage(probe, probe_phys, PTE_PRESENT | PTE_WRITABLE | PTE_NX);
        KernelVMM::UnmapPage(probe, false);
        PMM::FreeFrame(probe_phys);
    }

    g_inited = true;
    // NB: file logging stays OFF until EnableFileLog() is called (right after
    // KVFS::Init in kernel_main). kdf init runs BEFORE kvfs (so nvme, the first
    // migrated driver, can use kdf during the kvfs mount), so a LogDriver here
    // must only hit serial. (satoru)
    SerialLogger::Log("[KDF] kernel driver framework initialized (guard-page "
                      "isolation, higher-half window @ 0xFFFFC000_00000000)\r\n");
    LogDriver("kdf", "framework initialized");
}

int RegisterDriver(const char* name, KDFInitFn init) {
    if (!g_inited) Init();
    if (!name || !name[0]) return -1;
    if (FindDriver(name) >= 0) return FindDriver(name);  // idempotent (satoru)
    if (g_drv_count >= KDF_MAX_DRIVERS) return -1;
    int id = g_drv_count++;
    KDFDriver* d = &g_drv[id];
    for (int b = 0; b < (int)sizeof(KDFDriver); b++) ((char*)d)[b] = 0;
    d->in_use   = true;
    kd_cpy(d->name, name, sizeof(d->name));
    d->init     = init;
    d->state    = KDF_INIT;
    d->va_base  = KDF_VA_BASE + (uint64_t)id * KDF_DRIVER_VA_STRIDE;
    // leave the very first page of the slice as a permanent unmapped sentinel so
    // a null-ish low offset never lands on real memory. (satoru)
    d->va_cursor = d->va_base + KDF_PAGE_SIZE;
    d->region_count = 0;
    char m[64]; int p = 0;
    p = kd_cat(m, p, sizeof(m), "registered (window ");
    p = kd_cat_hex(m, p, sizeof(m), d->va_base);
    p = kd_cat(m, p, sizeof(m), ")");
    LogDriver(name, m);
    return id;
}

int FindDriver(const char* name) {
    if (!name) return -1;
    for (int i = 0; i < g_drv_count; i++)
        if (g_drv[i].in_use && kd_eq(g_drv[i].name, name)) return i;
    return -1;
}

// trampoline so Start() can run a driver's init() inside RunGuarded. (satoru)
namespace {
struct InitCtx { KDFInitFn fn; bool ok; };
void init_trampoline(void* arg) {
    InitCtx* c = (InitCtx*)arg;
    c->ok = c->fn ? c->fn() : true;
}
}  // namespace

bool Start(int driver_id) {
    if (driver_id < 0 || driver_id >= g_drv_count) return false;
    KDFDriver* d = &g_drv[driver_id];
    if (!d->in_use) return false;
    d->state = KDF_INIT;
    InitCtx ctx{ d->init, false };
    bool ran = RunGuarded(driver_id, init_trampoline, &ctx);
    if (ran && ctx.ok) {
        d->state = KDF_RUNNING;
        LogDriver(d->name, "init OK (running)");
        return true;
    }
    if (!ran) {
        // a fault during init already moved us to CRASHED + reported. (satoru)
        LogDriver(d->name, "init FAULTED (quarantined)");
        return false;
    }
    d->state = KDF_FAILED;
    LogDriver(d->name, "init returned failure");
    return false;
}

// ── the crash sandbox ────────────────────────────────────────────────────────
bool RunGuarded(int driver_id, KDFGuardedOp op, void* arg) {
    if (!op) return false;
    if (driver_id < 0 || driver_id >= g_drv_count) return false;

    PerCpuArm& a = this_cpu_arm();
    if (a.depth >= KDF_ARM_DEPTH) {
        // too deep: run unguarded rather than overflow (logged). a fault here
        // would panic, but this only triggers on pathological re-entry. (satoru)
        LogDriver(g_drv[driver_id].name, "RunGuarded nesting overflow (unguarded)");
        int prev = g_active_driver; g_active_driver = driver_id;
        op(arg);
        g_active_driver = prev;
        return true;
    }

    ArmedFrame* f = &a.stack[a.depth];
    f->driver_id = driver_id;
    f->faulted   = false;

    int prev_active = g_active_driver;
    g_active_driver = driver_id;
    a.depth++;

    // __builtin_setjmp returns 0 on the initial call, non-zero when longjmp'd
    // back from HandleGuardFault. (satoru)
    if (__builtin_setjmp(f->jb) == 0) {
        op(arg);
        // clean completion: pop the frame. (satoru)
        a.depth--;
        g_active_driver = prev_active;
        return true;
    }

    // we got here via longjmp from HandleGuardFault: the op faulted. the fault
    // was a cpu exception (#pf), which entered with interrupts DISABLED, and
    // __builtin_longjmp does NOT restore rflags  -  so IF is still 0 here. driver
    // code (and the scheduler it returns into) runs with interrupts enabled, so
    // re-enable them before unwinding. mirrors how the userspace resume path
    // lands back on an irqs-enabled kernel stack. (satoru)
    a.depth--;
    g_active_driver = prev_active;
    HAL::EnableInterrupts();
    return false;
}

// ── resource apis ────────────────────────────────────────────────────────────
namespace {
uint64_t pages_for(uint64_t size) {
    if (size == 0) size = 1;
    return (size + KDF_PAGE_SIZE - 1) / KDF_PAGE_SIZE;
}

// claim a region-record slot in the active driver: reuse the lowest freed
// (KDF_REGION_NONE) slot before growing, so a driver that cycles scratch buffers
// (FreeDMA) does not exhaust KDF_MAX_REGIONS. returns the slot index or -1. (satoru)
int claim_region_slot(KDFDriver* d) {
    for (int r = 0; r < d->region_count; r++)
        if (d->regions[r].kind == KDF_REGION_NONE) return r;
    if (d->region_count >= KDF_MAX_REGIONS) return -1;
    return d->region_count++;
}

// add a region record to the active driver. returns the user_va or nullptr. the
// payload phys must already be allocated; this maps it guard-fenced + records it.
// (satoru)
void* register_region(KDFRegionKind kind, uint64_t phys, uint64_t pages,
                      uint64_t usable_bytes) {
    if (g_active_driver < 0 || g_active_driver >= g_drv_count) return nullptr;
    KDFDriver* d = &g_drv[g_active_driver];
    int slot = claim_region_slot(d);
    if (slot < 0) return nullptr;
    uint64_t region_va = 0, total = 0;
    uint64_t user_va = carve_region(d, phys, pages, &region_va, &total);
    if (!user_va) return nullptr;
    KDFRegion* rg = &d->regions[slot];
    rg->kind        = kind;
    rg->region_va   = region_va;
    rg->user_va     = user_va;
    rg->phys        = phys;
    rg->bytes       = usable_bytes;
    rg->total_pages = total;
    rg->quarantined = false;
    return (void*)(uintptr_t)user_va;
}
}  // namespace

void FreeDMA(void* user_va) {
    if (!user_va) return;
    uint64_t va = (uint64_t)(uintptr_t)user_va;
    KDFDriver* d = nullptr;
    KDFRegion* rg = region_for_va(va, &d);
    if (!rg || !d) return;
    if (rg->kind != KDF_REGION_DMA && rg->kind != KDF_REGION_CONTIGUOUS) return;
    uint64_t payload = rg->total_pages >= 2 ? rg->total_pages - 2 : 0;
    if (!rg->quarantined) {
        for (uint64_t i = 0; i < payload; i++)
            KernelVMM::UnmapPage(rg->user_va + i * KDF_PAGE_SIZE, false);
        if (rg->phys && payload) PMM::FreeContiguous(rg->phys, payload);
    }
    // free the slot for reuse (leave the va window fenced/unmapped). (satoru)
    rg->kind        = KDF_REGION_NONE;
    rg->quarantined = false;
    rg->phys        = 0;
    rg->bytes       = 0;
    rg->user_va     = 0;
    rg->region_va   = 0;
    rg->total_pages = 0;
}

void* AllocDMA(uint64_t size) {
    uint64_t pages = pages_for(size);
    // contiguous physical frames so a single buffer is one dma target. (satoru)
    uint64_t phys = PMM::AllocContiguous(pages);
    if (!phys) return nullptr;
    // zero the payload via its identity alias (phys==identity-va below 16gb). (satoru)
    for (uint64_t i = 0; i < pages * KDF_PAGE_SIZE; i++)
        ((volatile uint8_t*)(uintptr_t)phys)[i] = 0;
    void* va = register_region(KDF_REGION_DMA, phys, pages, pages * KDF_PAGE_SIZE);
    if (!va) { PMM::FreeContiguous(phys, pages); return nullptr; }
    return va;
}

void* AllocContiguous(uint64_t size) {
    uint64_t pages = pages_for(size);
    uint64_t phys = PMM::AllocContiguous(pages);
    if (!phys) return nullptr;
    for (uint64_t i = 0; i < pages * KDF_PAGE_SIZE; i++)
        ((volatile uint8_t*)(uintptr_t)phys)[i] = 0;
    void* va = register_region(KDF_REGION_CONTIGUOUS, phys, pages, pages * KDF_PAGE_SIZE);
    if (!va) { PMM::FreeContiguous(phys, pages); return nullptr; }
    return va;
}

void* MapMMIO(uint64_t paddr, uint64_t size) {
    // page-align the base down and round the span up so the whole bar window is
    // covered; preserve the in-page offset in the returned pointer. (satoru)
    uint64_t base = paddr & ~(KDF_PAGE_SIZE - 1);
    uint64_t off  = paddr - base;
    uint64_t pages = pages_for(off + size);
    if (g_active_driver < 0 || g_active_driver >= g_drv_count) return nullptr;
    KDFDriver* d = &g_drv[g_active_driver];
    if (d->region_count >= KDF_MAX_REGIONS) return nullptr;

    // map the device bar into a guard-fenced higher-half window as uncached mmio.
    // we map the bar's PHYSICAL pages (not pmm-allocated) at a fresh kdf va, so
    // the driver touches a fenced alias; an oob access trips a guard page. (satoru)
    uint64_t total = pages + 2;
    uint64_t region_va = d->va_cursor;
    if (region_va + total * KDF_PAGE_SIZE > d->va_base + KDF_DRIVER_VA_STRIDE) return nullptr;
    uint64_t user_va = region_va + KDF_PAGE_SIZE;
    bool ok = true;
    for (uint64_t i = 0; i < pages; i++) {
        if (!KernelVMM::MapPage(user_va + i * KDF_PAGE_SIZE,
                                base + i * KDF_PAGE_SIZE,
                                PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_NX)) {
            ok = false; break;
        }
    }
    if (!ok) {
        for (uint64_t i = 0; i < pages; i++)
            KernelVMM::UnmapPage(user_va + i * KDF_PAGE_SIZE, false);
        return nullptr;
    }
    d->va_cursor = region_va + (total + 1) * KDF_PAGE_SIZE;
    KDFRegion* rg = &d->regions[d->region_count++];
    rg->kind        = KDF_REGION_MMIO;
    rg->region_va   = region_va;
    rg->user_va     = user_va;
    rg->phys        = base;
    rg->bytes       = pages * KDF_PAGE_SIZE;
    rg->total_pages = total;
    rg->quarantined = false;
    return (void*)(uintptr_t)(user_va + off);
}

uint64_t PhysOf(void* user_va) {
    uint64_t va = (uint64_t)(uintptr_t)user_va;
    KDFDriver* d = nullptr;
    KDFRegion* rg = region_for_va(va, &d);
    if (!rg) return 0;
    if (va < rg->user_va || va >= rg->user_va + rg->bytes) return 0;
    return rg->phys + (va - rg->user_va);
}

// ── crash-isolated irq (satoru) ──────────────────────────────────────────────
namespace {
// one trampoline slot per pic irq line; each remembers the owning driver + the
// real handler so the hal irq dispatch can route through RunGuarded. (satoru)
struct IrqSlot { int driver_id; KDFIrqHandler handler; };
IrqSlot g_irq[16] = {};

void irq_op(void* arg) {
    IrqSlot* s = (IrqSlot*)arg;
    if (s->handler) s->handler();
}

// the hal-facing thunks: a fixed thunk per line (HAL::IRQHandler takes an
// InterruptFrame*, which is a global struct). each runs its line's handler
// inside the crash sandbox. (satoru)
#define KDF_IRQ_THUNK(n) \
    void kdf_irq_thunk_##n(InterruptFrame*) { \
        if (g_irq[n].handler) RunGuarded(g_irq[n].driver_id, irq_op, &g_irq[n]); \
    }
KDF_IRQ_THUNK(0)  KDF_IRQ_THUNK(1)  KDF_IRQ_THUNK(2)  KDF_IRQ_THUNK(3)
KDF_IRQ_THUNK(4)  KDF_IRQ_THUNK(5)  KDF_IRQ_THUNK(6)  KDF_IRQ_THUNK(7)
KDF_IRQ_THUNK(8)  KDF_IRQ_THUNK(9)  KDF_IRQ_THUNK(10) KDF_IRQ_THUNK(11)
KDF_IRQ_THUNK(12) KDF_IRQ_THUNK(13) KDF_IRQ_THUNK(14) KDF_IRQ_THUNK(15)
#undef KDF_IRQ_THUNK
HAL::IRQHandler g_irq_thunks[16] = {
    kdf_irq_thunk_0,  kdf_irq_thunk_1,  kdf_irq_thunk_2,  kdf_irq_thunk_3,
    kdf_irq_thunk_4,  kdf_irq_thunk_5,  kdf_irq_thunk_6,  kdf_irq_thunk_7,
    kdf_irq_thunk_8,  kdf_irq_thunk_9,  kdf_irq_thunk_10, kdf_irq_thunk_11,
    kdf_irq_thunk_12, kdf_irq_thunk_13, kdf_irq_thunk_14, kdf_irq_thunk_15
};
}  // namespace

bool RegisterIRQ(int driver_id, uint8_t vector, KDFIrqHandler handler) {
    if (driver_id < 0 || driver_id >= g_drv_count) return false;
    if (vector >= 16) return false;
    g_irq[vector].driver_id = driver_id;
    g_irq[vector].handler   = handler;
    HAL::RegisterIRQHandler(vector, g_irq_thunks[vector]);
    HAL::EnableIRQ(vector);
    char m[48]; int p = 0;
    p = kd_cat(m, p, sizeof(m), "irq ");
    p = kd_cat_u(m, p, sizeof(m), vector);
    p = kd_cat(m, p, sizeof(m), " registered (crash-isolated)");
    LogDriver(g_drv[driver_id].name, m);
    return true;
}

// ── logging + crash reporting ────────────────────────────────────────────────
void LogDriver(const char* name, const char* msg) {
    char line[200]; int p = 0;
    p = kd_cat(line, p, sizeof(line), "[kdf] ");
    p = kd_cat(line, p, sizeof(line), name ? name : "-");
    p = kd_cat(line, p, sizeof(line), ": ");
    p = kd_cat(line, p, sizeof(line), msg ? msg : "");
    p = kd_cat(line, p, sizeof(line), "\r\n");
    SerialLogger::Log(line);

    // mirror to /kurono/var/log/drivers.log. KVFS is brought up early in boot
    // (before drivers) and Exists/Mkdirs/CreateFile are safe to call; this
    // mirrors kinit's LogEvent. g_log_to_fs gates out the pre-kvfs Init() call.
    // (satoru)
    if (g_log_to_fs) {
        const char* path = KP_LOG_DIR "/drivers.log";
        if (!KVFS::Exists(path)) { KVFS::Mkdirs(KP_LOG_DIR); KVFS::CreateFile(path); }
        // file form without the \r and the [kdf] serial prefix duplication. (satoru)
        char fline[200]; int q = 0;
        q = kd_cat(fline, q, sizeof(fline), name ? name : "-");
        q = kd_cat(fline, q, sizeof(fline), ": ");
        q = kd_cat(fline, q, sizeof(fline), msg ? msg : "");
        q = kd_cat(fline, q, sizeof(fline), "\n");
        KVFS::AppendFile(path, fline, (uint32_t)kd_len(fline));
    }
}

void ReportCrash(const char* name, const char* reason) {
    int id = FindDriver(name);
    if (id >= 0) {
        g_drv[id].state = KDF_CRASHED;
        g_drv[id].crash_count++;
    }
    char m[160]; int p = 0;
    p = kd_cat(m, p, sizeof(m), "CRASH: ");
    p = kd_cat(m, p, sizeof(m), reason ? reason : "(unspecified)");
    LogDriver(name, m);
    // notify kinit so it can run its restart/backoff policy. (satoru)
    if (g_crash_notify) g_crash_notify(name, reason);
}

// ── the fault hook ───────────────────────────────────────────────────────────
namespace {
void dump_regdump(const char* drv, uint64_t cr2, uint64_t rip) {
    char b[200]; int p = 0;
    p = kd_cat(b, p, sizeof(b), "[kdf] GUARD-FAULT driver=");
    p = kd_cat(b, p, sizeof(b), drv ? drv : "?");
    p = kd_cat(b, p, sizeof(b), " cr2=");
    p = kd_cat_hex(b, p, sizeof(b), cr2);
    p = kd_cat(b, p, sizeof(b), " rip=");
    p = kd_cat_hex(b, p, sizeof(b), rip);
    p = kd_cat(b, p, sizeof(b), " cpu=");
    p = kd_cat_u(b, p, sizeof(b), SMP::CpuIndex());
    p = kd_cat(b, p, sizeof(b), "\r\n");
    SerialLogger::Log(b);
}

// unmap every payload+guard page of a region so a re-fault can't loop and the
// stale buffer is gone. dma frames are freed back to pmm; mmio is just unmapped
// (we don't own the device bar). (satoru)
void quarantine_region(KDFDriver* d, KDFRegion* rg) {
    if (!rg || rg->quarantined) return;
    uint64_t pages_payload = rg->total_pages >= 2 ? rg->total_pages - 2 : 0;
    // unmap the payload pages (and the lead/trail guards are already unmapped). (satoru)
    for (uint64_t i = 0; i < pages_payload; i++)
        KernelVMM::UnmapPage(rg->user_va + i * KDF_PAGE_SIZE, false);
    if (rg->kind == KDF_REGION_DMA || rg->kind == KDF_REGION_CONTIGUOUS) {
        if (rg->phys && pages_payload) PMM::FreeContiguous(rg->phys, pages_payload);
    }
    rg->quarantined = true;
    char m[96]; int p = 0;
    p = kd_cat(m, p, sizeof(m), "quarantined region va=");
    p = kd_cat_hex(m, p, sizeof(m), rg->user_va);
    p = kd_cat(m, p, sizeof(m), " (");
    p = kd_cat_u(m, p, sizeof(m), pages_payload);
    p = kd_cat(m, p, sizeof(m), " pages unmapped)");
    LogDriver(d ? d->name : "?", m);
}
}  // namespace

bool HandleGuardFault(uint64_t cr2, uint64_t rip) {
    if (!g_inited) return false;
    if (!is_kdf_va(cr2)) return false;   // not ours: let the kernel fault path run (satoru)

    KDFDriver* d = nullptr;
    KDFRegion* rg = region_for_va(cr2, &d);
    const char* drv_name = d ? d->name : "kdf-unknown";

    dump_regdump(drv_name, cr2, rip);
    if (d) { d->last_fault_addr = cr2; d->last_fault_rip = rip; }
    if (rg) quarantine_region(d, rg);

    // record + report the crash (sets state CRASHED, bumps count, notifies kinit).
    // (satoru)
    char reason[96]; int p = 0;
    p = kd_cat(reason, p, sizeof(reason), "guard-page fault at ");
    p = kd_cat_hex(reason, p, sizeof(reason), cr2);
    ReportCrash(drv_name, reason);

    // unwind to the innermost armed RunGuarded on THIS cpu, if any. (satoru)
    PerCpuArm& a = this_cpu_arm();
    if (a.depth > 0) {
        ArmedFrame* f = &a.stack[a.depth - 1];
        f->faulted = true;
        // longjmp re-enters RunGuarded's setjmp with a non-zero value; it pops
        // the frame + returns false. DOES NOT RETURN here. (satoru)
        __builtin_longjmp(f->jb, 1);
    }

    // kdf address but no armed sandbox to unwind to (e.g. a stray pointer outside
    // any RunGuarded). we have already quarantined + logged + reported; tell the
    // caller we handled it so the kernel does not panic. the offending code path
    // will simply continue/return; the region is gone so it cannot re-fault into
    // freed memory. (satoru)
    return true;
}

// ── status / introspection ───────────────────────────────────────────────────
KDFState GetState(int id) {
    if (id < 0 || id >= g_drv_count) return KDF_UNUSED;
    return g_drv[id].state;
}

const char* StateName(KDFState s) {
    switch (s) {
        case KDF_UNUSED:     return "unused";
        case KDF_INIT:       return "init";
        case KDF_RUNNING:    return "running";
        case KDF_CRASHED:    return "crashed";
        case KDF_RESTARTING: return "restarting";
        case KDF_FAILED:     return "failed";
        default:             return "?";
    }
}

int GetDriverCount() { return g_drv_count; }

bool GetCrashInfo(int id, KDFCrashInfo* out) {
    if (!out) return false;
    if (id < 0 || id >= g_drv_count) { out->valid = false; return false; }
    KDFDriver* d = &g_drv[id];
    kd_cpy(out->driver, d->name, sizeof(out->driver));
    out->fault_addr  = d->last_fault_addr;
    out->fault_rip   = d->last_fault_rip;
    out->crash_count = d->crash_count;
    out->valid       = true;
    return true;
}

uint64_t DriverMappedBytes(int id) {
    if (id < 0 || id >= g_drv_count) return 0;
    KDFDriver* d = &g_drv[id];
    uint64_t total = 0;
    for (int r = 0; r < d->region_count; r++)
        if (!d->regions[r].quarantined) total += d->regions[r].bytes;
    return total;
}

void SetRunning(int id) {
    if (id < 0 || id >= g_drv_count) return;
    g_drv[id].state = KDF_RUNNING;
}
void SetRestarting(int id) {
    if (id < 0 || id >= g_drv_count) return;
    g_drv[id].state = KDF_RESTARTING;
}

int Status(char* out, int mx) {
    if (!out || mx < 2) return 0;
    int p = 0;
    p = kd_cat(out, p, mx, "KDF drivers (guard-page isolated, ring 0+):\n");
    for (int i = 0; i < g_drv_count; i++) {
        KDFDriver* d = &g_drv[i];
        if (!d->in_use) continue;
        p = kd_cat(out, p, mx, "  ");
        p = kd_cat(out, p, mx, d->name);
        p = kd_cat(out, p, mx, "  state=");
        p = kd_cat(out, p, mx, StateName(d->state));
        p = kd_cat(out, p, mx, "  regions=");
        p = kd_cat_u(out, p, mx, (uint64_t)d->region_count);
        p = kd_cat(out, p, mx, "  mapped=");
        p = kd_cat_u(out, p, mx, DriverMappedBytes(i) / 1024);
        p = kd_cat(out, p, mx, "KiB  crashes=");
        p = kd_cat_u(out, p, mx, d->crash_count);
        p = kd_cat(out, p, mx, "\n");
    }
    return p;
}

}  // namespace KDF

// end (satoru)
