#pragma once
#include "types.h"

//  kurono os: kdf, the kernel driver framework (kurono's answer to windows kmdf).
//
//  kdf is the ring-0+ sandbox layer of the hybrid-kernel architecture. the
//  perf-critical core (vmm/pmm/scheduler/compositor/nvme-fastpath/tcp) stays in
//  plain ring 0; a kdf-managed driver still runs in ring 0 (same privilege, same
//  cr3) but every dma buffer and mmio window it obtains through kdf is fenced by
//  unmapped GUARD PAGES before and after it. an out-of-bounds dereference walks
//  off the buffer into a guard page, the cpu raises a #pf, and the kdf fault
//  hook (called from hal.cpp) recognizes the address as belonging to a kdf
//  region, QUARANTINES that region (unmaps it), dumps registers, reports the
//  crash to kinit, and longjmps the faulting driver operation back to its
//  KDF::RunGuarded() call-site with an error, instead of panicking the kernel.
//  kinit then restarts the driver.
//
//  HONEST SCOPE (documented, not hidden): this is VMM guard-page isolation, NOT
//  full address-space separation. a kdf driver shares the kernel page tables, so
//  a wild write to an arbitrary in-range kernel address is NOT caught (only the
//  fenced guard pages around its own dma/mmio are). it is the same isolation the
//  ksa/kinit guard-page primitives (IsolateFrames) already use, extended into a
//  per-driver allocator + a crash-recovery path. it catches the common real bug
//  (a driver running off the end of a ring/descriptor/dma buffer) and keeps the
//  os alive, which is the point. (satoru)

namespace KDF {

constexpr int      KDF_MAX_DRIVERS   = 32;
constexpr int      KDF_NAME_LEN      = 24;
constexpr int      KDF_MAX_REGIONS   = 24;   // dma + mmio regions per driver (satoru)
constexpr uint64_t KDF_PAGE_SIZE     = 4096ULL;

// the higher-half virtual window kdf carves driver regions out of. far above the
// identity map (low ~16gb) and the user canonical lower half, so a fault here is
// unambiguously a kdf guard-page hit. each driver gets a KDF_DRIVER_VA_STRIDE
// slice; guard pages are unmapped holes inside that slice. (satoru)
constexpr uint64_t KDF_VA_BASE          = 0xFFFFC00000000000ULL;
constexpr uint64_t KDF_DRIVER_VA_STRIDE = 0x0000000040000000ULL;  // 1 GiB per driver (satoru)

// driver lifecycle / health state tracked by kdf (mirrors kinit's view but is
// the kdf-side truth used by the crash path). (satoru)
enum KDFState : uint8_t {
    KDF_UNUSED = 0,
    KDF_INIT,        // registered, init in progress (satoru)
    KDF_RUNNING,     // healthy (satoru)
    KDF_CRASHED,     // a guard-page fault tripped; awaiting restart (satoru)
    KDF_RESTARTING,  // restart requested, re-init pending (satoru)
    KDF_FAILED       // gave up after too many crashes (satoru)
};

// what kind of region a driver mapping is, for the quarantine + log path. (satoru)
enum KDFRegionKind : uint8_t {
    KDF_REGION_NONE = 0,
    KDF_REGION_DMA,         // AllocDMA: kdf-owned pmm frames, guard-fenced (satoru)
    KDF_REGION_CONTIGUOUS,  // AllocContiguous: physically contiguous + guard-fenced (satoru)
    KDF_REGION_MMIO         // MapMMIO: a device bar window, guard-fenced (satoru)
};

// one fenced region. `user_va` is the address handed back to the driver (the
// first usable byte, one guard page in from region_va). (satoru)
struct KDFRegion {
    KDFRegionKind kind;
    uint64_t      region_va;    // base of the whole slice incl. lead guard page (satoru)
    uint64_t      user_va;      // first usable byte returned to the driver (satoru)
    uint64_t      phys;         // backing physical base (== user_va's phys) (satoru)
    uint64_t      bytes;        // usable size (page-rounded) (satoru)
    uint64_t      total_pages;  // lead guard + payload + trail guard (satoru)
    bool          quarantined;  // unmapped after a fault (satoru)
};

// crash record for diagnostics + kinit. (satoru)
struct KDFCrashInfo {
    char     driver[KDF_NAME_LEN];
    uint64_t fault_addr;        // cr2 at the guard fault (satoru)
    uint64_t fault_rip;         // rip at the fault (satoru)
    uint32_t crash_count;       // total crashes for this driver (satoru)
    bool     valid;
};

// ── lifecycle ────────────────────────────────────────────────────────────────
void Init();                    // reserve the kdf va window; called once, early (satoru)

// register a driver context. `init` is the driver's (re-)init entry: kdf calls
// it on first bring-up via Start() and again after a crash. may be null for a
// driver that re-inits via kinit instead. returns a driver id >= 0, or -1. (satoru)
typedef bool (*KDFInitFn)();
int  RegisterDriver(const char* name, KDFInitFn init);
int  FindDriver(const char* name);

// run a driver's init entry inside the crash sandbox. returns true if it
// initialized cleanly, false if it faulted or its init returned false. (satoru)
bool Start(int driver_id);

// ── the crash sandbox (satoru) ───────────────────────────────────────────────
// RunGuarded executes `op` as driver `driver_id` with crash isolation armed: a
// guard-page #pf anywhere inside this call unwinds back here and returns false
// (the region is quarantined + the crash reported). returns true if op ran to
// completion. NESTING is supported (saves/restores the previous armed context).
// op is a plain function pointer + opaque arg to stay freestanding-friendly.
// (satoru)
typedef void (*KDFGuardedOp)(void* arg);
bool RunGuarded(int driver_id, KDFGuardedOp op, void* arg);

// ── resource apis the driver calls (only valid inside its init / RunGuarded) ──
// every allocation is fenced by an unmapped guard page on each side. (satoru)
void* AllocDMA(uint64_t size);                 // page-aligned dma buffer (satoru)
void* AllocContiguous(uint64_t size);          // physically-contiguous + fenced (satoru)
void* MapMMIO(uint64_t paddr, uint64_t size);  // bounds-checked device bar window (satoru)

// release a region previously returned by AllocDMA/AllocContiguous: unmaps its
// payload, frees its backing frames, and frees the region slot for reuse (so a
// driver that cycles scratch buffers does not exhaust KDF_MAX_REGIONS). a no-op
// for a va that is not a live kdf dma region (mmio is not freed here; it is
// reclaimed on quarantine / teardown). (satoru)
void FreeDMA(void* user_va);

// translate a kdf user_va back to its backing physical address (for dma program-
// ming: prp/descriptor base). returns 0 if the va is not a live kdf region. the
// payload is physically contiguous so phys+offset is valid across the buffer.
// (satoru)
uint64_t PhysOf(void* user_va);

// register a crash-isolated irq handler for `vector` (a pic irq line 0..15). the
// real handler runs inside the owning driver's sandbox, so a fault in an isr
// quarantines + reports instead of panicking. (satoru)
typedef void (*KDFIrqHandler)();
bool RegisterIRQ(int driver_id, uint8_t vector, KDFIrqHandler handler);

// append a line to /kurono/var/log/drivers.log (and mirror to serial). (satoru)
void LogDriver(const char* name, const char* msg);

// report a driver crash: records it, logs it, and notifies kinit so it can
// restart the driver. called by the fault path; also callable directly by a
// driver that detects an unrecoverable hardware state. (satoru)
void ReportCrash(const char* name, const char* reason);

// install kinit's crash-notify callback. kdf calls it from ReportCrash with the
// crashed driver's name + reason; kinit then runs its restart/backoff policy.
// kept as a registered hook so kdf.cpp does not hard-depend on kinit (avoids an
// init-order / link cycle). (satoru)
void SetCrashNotifier(void (*fn)(const char* driver, const char* reason));

// enable mirroring driver-log lines into /kurono/var/log/drivers.log. call once
// kvfs is up (kdf.Init runs before kvfs so nvme can use kdf during the mount).
// (satoru)
void EnableFileLog();

// ── fault-path hook (called from hal.cpp's #pf handler) ──────────────────────
// if `cr2` lands in any live kdf region's guard page (or any quarantined kdf
// page), handle it: quarantine the region, dump regs, report to kinit, and
// longjmp the active RunGuarded()/Start() back to its call-site. DOES NOT RETURN
// when it handles a fault on the armed cpu (it longjmps). returns false if cr2 is
// not a kdf address (the normal kernel-fault path then proceeds to panic). a true
// return with no active sandbox means "kdf address but nothing to unwind to"  - 
// the caller still must not panic; kdf has already quarantined + logged. (satoru)
bool HandleGuardFault(uint64_t cr2, uint64_t rip);

// ── status / introspection (satoru) ──────────────────────────────────────────
KDFState    GetState(int driver_id);
const char* StateName(KDFState s);
int         GetDriverCount();
bool        GetCrashInfo(int driver_id, KDFCrashInfo* out);
// render a human status table (driver, state, regions, dma kib, crashes) into out.
// (satoru)
int         Status(char* out, int mx);
// total bytes of dma+mmio kdf currently has fenced for a driver. (satoru)
uint64_t    DriverMappedBytes(int driver_id);

// mark a driver healthy/crashed from outside (kinit restart bookkeeping). (satoru)
void        SetRunning(int driver_id);
void        SetRestarting(int driver_id);

// ── crash-recovery self-test (kurono.kdf.test gate; kdf_test.cpp) (satoru) ────
// register a deliberately-faulting test driver as a supervised kinit kdf unit.
// call before KInit::Boot. (satoru)
void RegisterCrashTestDriver();
// power off after the test finishes (kurono.kdf.poweroff). (satoru)
void SetCrashTestPoweroff(bool v);
// run the 3 crash-recovery scenarios (sandbox unwind / kinit restart / in-bounds
// still-works) and log PASS/FAIL to serial. call from a kernel-process after the
// desktop is up so kinit's monitor is ticking. (satoru)
void RunCrashRecoveryTest();

}  // namespace KDF

// end (satoru)
