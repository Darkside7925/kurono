#pragma once
#include "types.h"

//  KMemX  -  Kurono Memory Compression Engine. (satoru)
//
//  compresses inactive 4kb pages across the whole os into a fixed physical pool
//  using lz4 (see kmemx_lz4.*), so a page that has not been touched for a while
//  costs ~1.5-2 kb of physical ram instead of 4 kb, and is transparently
//  decompressed on the page-fault that next touches it. a dedicated, cpu-capped
//  kernel process scans for candidates; the fault path serves decompressions.
//
//  this header is the engine's public surface. the layered build is:
//    stage 2  pool allocator + flat metadata table + stats   (this file)
//    stage 3  vmm page-aging (generation counter in pte bits)
//    stage 4  the kmemx kernel process (token bucket, 10ms tick)
//    stage 5  compress_inactive_pages() for native processes
//    stage 6  page-fault decompression path (<5us)
//    ...      pressure, guests, dedup, toggle, settings, shell
//
//  safety is non-negotiable: every page is crc32'd before compression and the
//  crc is re-verified on decompress; a mismatch is silent memory corruption and
//  panics the kernel. the never-compress list (kernel core, page tables, the
//  compositor framebuffer, active dma rings, ksa ept regions, interrupt stacks,
//  and kmemx's own pool/metadata) is enforced in IsCompressible(). (satoru)

namespace KMemX {

// ── pressure levels (stage 8 drives cpu budget + thresholds off these) ──────
enum Pressure : uint8_t {
    PRESS_GREEN = 0,   // > 50% free: 1% cpu, compress oldest only (satoru)
    PRESS_YELLOW,      // 30-50% free: 2% cpu, threshold 6 (satoru)
    PRESS_ORANGE,      // 15-30% free: 4% cpu, threshold 4, compress guests (satoru)
    PRESS_RED,         // 5-15% free: 5% cpu, threshold 2, all guests compressed (satoru)
    PRESS_CRITICAL     // < 5% free: emergency, compress everything safe (satoru)
};

// ── per-page metadata (flat cache-friendly array; no pointer chasing) ───────
// one entry per compressed page. the table is a fixed array sized at init from
// the pool size; lookups on the fault path hash (address_space, vaddr). (satoru)
struct PageMeta {
    uint64_t address_space;   // owning pml4 physical (cr3)  -  0 = free slot (satoru)
    uint64_t vaddr;           // page-aligned virtual address in that space (satoru)
    uint32_t pool_off;        // byte offset of the compressed blob in the pool (satoru)
    uint16_t comp_size;       // compressed byte count (<= 4096; 4096 == stored raw) (satoru)
    uint16_t flags;           // KMETA_* (satoru)
    uint32_t crc32;           // crc32 of the ORIGINAL 4kb page (verified on decomp) (satoru)
    uint32_t orig_pte_flags;  // the leaf pte permission/attr bits to restore (satoru)
    uint32_t dedup_refs;      // shared-page reference count (>1 == deduped COW) (satoru)
    uint8_t  generation;      // age at compression time (diagnostic) (satoru)
    uint8_t  lock_bit;        // 1 while a fault is decompressing this entry (satoru)
    uint16_t _pad;
};

constexpr uint16_t KMETA_NONE      = 0;
constexpr uint16_t KMETA_RAW       = 1 << 0;  // stored uncompressed (incompressible) (satoru)
constexpr uint16_t KMETA_GUEST     = 1 << 1;  // a hypervisor ept guest page (satoru)
constexpr uint16_t KMETA_DEDUP     = 1 << 2;  // merged into a shared cow blob (satoru)
constexpr uint16_t KMETA_DEDUP_HEAD= 1 << 3;  // owns the shared blob's pool extent (satoru)

// ── pool + engine stats (read by shell `kmemx stats` and Settings) ──────────
struct Stats {
    uint64_t pages_in;        // total pages compressed-in over the engine's life (satoru)
    uint64_t pages_out;       // total pages decompressed-out (faults served) (satoru)
    uint64_t faults_served;   // page faults resolved from the pool (satoru)
    uint64_t bytes_saved;     // sum over live pages of (4096 - comp_size) (satoru)
    uint64_t pool_bytes;      // total pool size in bytes (satoru)
    uint64_t pool_used;       // bytes of the pool currently holding blobs (satoru)
    uint32_t live_pages;      // entries currently compressed (satoru)
    uint32_t dedup_saved;     // pages eliminated by dedup (shared) (satoru)
    uint32_t panics_avoided;  // crc mismatches caught (always panics; diag) (satoru)
    uint32_t compress_fail;   // pages that did not fit / declined (stored raw) (satoru)
    uint64_t ns_compress_max; // worst single-page compress time (ns) (satoru)
    uint64_t ns_decompress_max; // worst single-page decompress time (ns) (satoru)
    uint64_t ns_decompress_min; // best single-page decompress time (ns)  -  warm path (satoru)
    uint64_t ns_decompress_sum; // running sum for the mean decompress latency (satoru)
    uint64_t decomp_over_10us;  // decompressions that blew the 10us invisibility budget (satoru)
};

// ── lifecycle ───────────────────────────────────────────────────────────────
// reserve the pool (pct of total ram, clamped 10..40) + allocate the metadata
// table + the compress/decompress scratch buffers. idempotent. does NOT start
// the kernel process. returns false if the pmm could not satisfy the pool. (satoru)
bool Init(int pool_pct);

// true once Init() has reserved the pool. (satoru)
bool IsInitialized();

// is the engine ENABLED (user toggle / config). when disabled the process does
// not scan and the fault path is a no-op (nothing is ever compressed). (satoru)
bool IsEnabled();
void SetEnabled(bool on);   // stage 11 toggle drives this (satoru)

// ── the compress / decompress primitives (stages 5-6 build on these) ────────
// compress the single 4kb page mapped at `vaddr` in address space `as` and,
// on success, store the blob in the pool, record metadata, and make the pte
// not-present so the next access faults. returns true if the page was taken
// into the pool (compressed OR stored-raw); false if it was skipped (not
// mapped, on the never-compress list, table/pool full, or already compressed).
// (satoru)
bool CompressPage(uint64_t as, uint64_t vaddr);

// the page-fault entry: if `vaddr` in the CURRENT address space is a compressed
// page, decompress it back into a fresh frame, restore the pte, free the pool
// slot + metadata, and return true (the faulting instruction is then retried).
// returns false if the address is not ours (the normal fault path continues).
// crc32 is verified here; a mismatch panics. designed for < 5us. (satoru)
bool HandleFault(uint64_t fault_vaddr);

// is this (address_space,vaddr) page safe to compress? enforces the
// never-compress list. exposed so the scanner can pre-filter cheaply. (satoru)
bool IsCompressible(uint64_t as, uint64_t vaddr);

// register a physical range that must NEVER be compressed (dma rings, the
// framebuffer, ept regions, interrupt stacks, ...). called by the owning
// subsystem at init. ranges are matched by the page's BACKING phys frame. (satoru)
void ReserveNeverCompress(uint64_t phys_base, uint64_t bytes, const char* who);

// ── scanning / aging (stages 3-5) ───────────────────────────────────────────
// run one scan pass over candidate address spaces, compressing up to `budget`
// pages whose generation exceeds the current threshold. returns pages taken.
// honours the per-tick token bucket via the caller. (satoru)
int ScanAndCompress(int budget);

// the generation threshold above which a page becomes a candidate, derived from
// the live pressure level (configurable base via SetThreshold). (satoru)
int Threshold();
void SetThreshold(int gen);   // settings aggressiveness slider 4..16 (satoru)

// ── pressure (stage 8) ──────────────────────────────────────────────────────
Pressure CurrentPressure();
const char* PressureName(Pressure p);
// recompute pressure from live pmm free %; called every tick by the process
// and every second by kinit. returns the (possibly changed) level. (satoru)
Pressure UpdatePressure();

// kinit calls this once per second. it recomputes pressure and drives the
// cross-system reactions the engine owns: at Orange+ it aggressively compresses
// guest VM pages (signalling them to release memory); the level it returns lets
// kinit decide whether to stop launching services (Red) or shed the
// lowest-priority ones (Critical). logs a level transition to serial. (satoru)
Pressure PressureTick();

// true when memory pressure is high enough (Red or Critical) that kinit should
// NOT launch new services. (satoru)
bool ShouldBlockNewServices();

// true at Critical: kinit should gracefully terminate its lowest-priority
// services to free memory. (satoru)
bool ShouldShedServices();

// ── pool sizing (settings slider 10..40%) ───────────────────────────────────
int  PoolPct();
// resize the pool to a new pct. only grows/shrinks when safe (no live pages in
// the shrunk-away tail); returns true on success. (satoru)
bool SetPoolPct(int pct);

// ── stats ────────────────────────────────────────────────────────────────────
const Stats& GetStats();
// effective compression ratio x100 (e.g. 250 == 2.5:1) over live pages. (satoru)
int RatioX100();

// ── bulk operations (shell `kmemx compress/decompress/flush`) ────────────────
// compress all eligible pages of one process (by linux pid or kernel pid). (satoru)
int CompressProcess(uint32_t pid);
// decompress every live page belonging to one process (restores its working set). (satoru)
int DecompressProcess(uint32_t pid);
// decompress EVERY live page (used by `kmemx disable` / `flush`). drains the
// whole pool back into physical ram. returns pages restored. (satoru)
int DecompressAll();

// ── dedup (stage 10) ─────────────────────────────────────────────────────────
// one dedup pass: find compressed pages with identical crc32 + identical bytes
// and merge them into a single shared cow blob. returns pages eliminated. (satoru)
int DedupPass(int budget);

// ── guests (stage 9) ─────────────────────────────────────────────────────────
// register the hypervisor's ept root + a guest's name so guest pages can be
// compressed at the ept level. the ept root is passed as a host pointer cast to
// uint64_t (EPT_PML4*). (satoru)
void RegisterGuest(uint64_t ept_root, const char* name);
void UnregisterGuest(uint64_t ept_root);

// compress up to `budget` idle guest-physical pages across all registered guests
// at the hypervisor ept level (the guest has zero knowledge). returns pages
// taken. called by PressureTick at Orange+ and by `kmemx compress --guests`. on
// a host with no running guest this is a no-op. (satoru)
int CompressGuests(int budget);

// the ept-violation entry: when a guest touches a page kmemx compressed out at
// the ept level, the hypervisor's ept-violation VMEXIT calls this with the
// faulting guest-physical address. if it is one of ours, decompress it back into
// a fresh frame, restore the ept mapping, and return true so the guest
// instruction is retried. returns false if not ours. crc-verified; mismatch
// panics. (satoru)
bool HandleGuestFault(uint64_t ept_root, uint64_t guest_phys);

// convenience for the ept-violation handler, which sees the guest-physical
// address but not which ept root it belongs to: try every registered guest's
// ept root. returns true if any served the fault. inert with no guest. (satoru)
bool HandleGuestFaultAny(uint64_t guest_phys);

// ── kernel process (stage 4) ─────────────────────────────────────────────────
// spawn the dedicated, cpu-capped kmemx compression kernel-process. it wakes
// every 10ms, acquires a token-bucket budget (1-16 pages/tick by pressure),
// yields immediately if the compositor is rendering, scans for aged candidates,
// and runs a low-frequency dedup pass. highest kernel-process priority below
// the scheduler. returns true if spawned. only starts if IsEnabled(). (satoru)
bool StartProcess();
// true once the process has been spawned this boot. (satoru)
bool ProcessRunning();

// the per-tick page budget derived from the live pressure level (1..16). the
// token bucket caps the process to this many compressions per 10ms tick so cpu
// stays at 1-5% regardless of load. exposed for the shell `kmemx status`. (satoru)
int TokenBudget();

}  // namespace KMemX

// end (satoru)
