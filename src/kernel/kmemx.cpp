#include "kmemx.h"
#include "kmemx_internal.h"
#include "kmemx_lz4.h"
#include "kmemx_pool.h"
#include "pmm.h"
#include "vmm.h"
#include "panic.h"
#include "../drivers/serial.h"
#include "../drivers/cpu_detect.h"
#include "../proc/spinlock.h"
#include "../proc/scheduler.h"
#include "../virt/ept.h"          // ept leaf walk for hypervisor guest-page compression (satoru)

//  KMemX engine core. stage 2: pool + flat metadata table + stats + the
//  never-compress list + the byte-level store/retrieve primitives. the pte
//  manipulation in CompressPage/HandleFault is completed in stages 3 (aging)
//  and 6 (fault path); here they use the primitives so the stage-2 pool
//  self-test can round-trip 1000 compressed pages through the real pool +
//  metadata table. (satoru)

namespace KMemX {

namespace {

// ── engine state ─────────────────────────────────────────────────────────────
bool      g_inited   = false;
bool      g_enabled  = false;       // user toggle (stage 11) - off until enabled (satoru)
int       g_pool_pct = 20;          // default 20% of ram (satoru)
int       g_threshold = 8;          // base generation threshold (satoru)
Pressure  g_pressure = PRESS_GREEN;
Stats     g_stats;
Spinlock  g_lock;                   // protects the metadata table (satoru)

// ── flat metadata table (open-addressed hash on (as,vaddr)) ─────────────────
// sized at init to ~ (pool_bytes / 1500) entries (avg blob ~1.5kb) rounded up to
// a power of two, capped. open addressing keeps it pointer-free + cache-friendly
// for the fault-path lookup. (satoru)
PageMeta* g_meta      = nullptr;
uint32_t  g_meta_cap  = 0;          // power-of-two table size (satoru)
uint32_t  g_meta_mask = 0;
uint32_t  g_meta_live = 0;

// ── compress/decompress scratch (pre-allocated; never malloc'd on hot path) ──
uint8_t*  g_scratch_hash = nullptr;   // lz4 compressor hash table (satoru)
uint8_t*  g_scratch_comp = nullptr;   // CompressBound(4096) staging buffer (satoru)
uint8_t*  g_scratch_dda  = nullptr;   // dedup decompress buffer A (4kb) (satoru)
uint8_t*  g_scratch_ddb  = nullptr;   // dedup decompress buffer B (4kb) (satoru)
constexpr int PAGE = 4096;

// ── never-compress physical ranges ──────────────────────────────────────────
struct NeverRange { uint64_t base; uint64_t end; const char* who; };
constexpr int MAX_NEVER = 64;
NeverRange g_never[MAX_NEVER];
int        g_never_count = 0;

// ── guest ept roots (stage 9) ────────────────────────────────────────────────
struct GuestReg { uint64_t ept_root; const char* name; };
constexpr int MAX_GUESTS = 8;
GuestReg g_guests[MAX_GUESTS];
int      g_guest_count = 0;

// ── dedup shared-extent refcounts (stage 10) ────────────────────────────────
// when two compressed pages have identical bytes, dedup points both PageMetas
// at ONE pool extent and the second's frame/extent is freed. a shared extent
// must not be freed back to the pool until the LAST referencing page faults in,
// so its reference count lives here, keyed by pool_off (the blob's stable
// handle). a small open-addressed table; only deduped extents (refs >= 2)
// occupy an entry - a normal 1:1 page never touches it. (satoru)
struct DedupRef { uint32_t pool_off; uint32_t refs; };
constexpr int MAX_DEDUP = 4096;
DedupRef g_dedup[MAX_DEDUP];
int      g_dedup_count = 0;   // live shared-extent entries (diagnostic) (satoru)

// find the refcount entry for a shared pool_off, or -1. (satoru)
static int dedup_find(uint32_t pool_off) {
    for (int i = 0; i < MAX_DEDUP; i++)
        if (g_dedup[i].refs != 0 && g_dedup[i].pool_off == pool_off) return i;
    return -1;
}
// get-or-create the refcount entry for a shared extent, seeded to `init`. (satoru)
static int dedup_intern(uint32_t pool_off, uint32_t init) {
    int idx = dedup_find(pool_off);
    if (idx >= 0) return idx;
    for (int i = 0; i < MAX_DEDUP; i++) {
        if (g_dedup[i].refs == 0) {
            g_dedup[i].pool_off = pool_off;
            g_dedup[i].refs = init;
            g_dedup_count++;
            return i;
        }
    }
    return -1;   // table full -> dedup declines this merge (satoru)
}

// fnv-1a-ish hash of (as,vaddr) -> table slot. (satoru)
static inline uint32_t meta_hash(uint64_t as, uint64_t vaddr) {
    uint64_t h = 1469598103934665603ULL;
    h = (h ^ (as >> 12)) * 1099511628211ULL;
    h = (h ^ (vaddr >> 12)) * 1099511628211ULL;
    return (uint32_t)(h ^ (h >> 32)) & g_meta_mask;
}

// find the slot holding (as,vaddr), or -1. open-addressed linear probe. (satoru)
static int meta_find(uint64_t as, uint64_t vaddr) {
    if (!g_meta) return -1;
    uint32_t i = meta_hash(as, vaddr);
    for (uint32_t probe = 0; probe <= g_meta_mask; probe++) {
        PageMeta& m = g_meta[i];
        if (m.address_space == 0 && m.pool_off == KMEMX_POOL_NULL) return -1;  // empty -> not present (satoru)
        if (m.address_space == as && m.vaddr == vaddr && m.pool_off != KMEMX_POOL_NULL) return (int)i;
        i = (i + 1) & g_meta_mask;
    }
    return -1;
}

// claim a free slot for (as,vaddr); returns slot or -1 if the table is full. a
// slot is free if address_space==0 AND pool_off==NULL (a tombstone keeps
// address_space==0 but pool_off!=NULL... we don't tombstone - we backfill on
// delete, see meta_erase). (satoru)
static int meta_insert(uint64_t as, uint64_t vaddr) {
    if (!g_meta) return -1;
    uint32_t i = meta_hash(as, vaddr);
    for (uint32_t probe = 0; probe <= g_meta_mask; probe++) {
        PageMeta& m = g_meta[i];
        if (m.pool_off == KMEMX_POOL_NULL) {   // free (or freshly-erased) slot (satoru)
            m.address_space = as;
            m.vaddr = vaddr;
            return (int)i;
        }
        i = (i + 1) & g_meta_mask;
    }
    return -1;
}

// erase slot `idx` and repair the probe chain (robin-hood-free backward shift)
// so meta_find never stops early at a hole. (satoru)
static void meta_erase(int idx) {
    if (idx < 0) return;
    uint32_t i = (uint32_t)idx;
    g_meta[i].address_space = 0;
    g_meta[i].vaddr = 0;
    g_meta[i].pool_off = KMEMX_POOL_NULL;
    // shift subsequent entries back into the hole if they probed past it. (satoru)
    uint32_t j = (i + 1) & g_meta_mask;
    while (g_meta[j].pool_off != KMEMX_POOL_NULL) {
        uint32_t home = meta_hash(g_meta[j].address_space, g_meta[j].vaddr);
        // is `home` cyclically within (i, j]? if so, j can move back to i. (satoru)
        bool can_move;
        if (i <= j) can_move = !(home > i && home <= j);
        else        can_move = !(home > i || home <= j);
        if (can_move) {
            g_meta[i] = g_meta[j];
            g_meta[j].address_space = 0;
            g_meta[j].vaddr = 0;
            g_meta[j].pool_off = KMEMX_POOL_NULL;
            i = j;
        }
        j = (j + 1) & g_meta_mask;
    }
}

static uint32_t next_pow2(uint32_t v) {
    uint32_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

// ── timing (rdtsc -> ns for the latency stats + invisibility budget) ────────
static inline uint64_t rdtsc() {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
// tsc ticks per microsecond (cached; pre-warmed in Init so the fault path never
// pays the CPUDetect::GetInfo() cost). guards against a 0 freq. (satoru)
static uint64_t g_tsc_per_us = 0;
static inline void timing_prewarm() {
    if (g_tsc_per_us == 0) {
        uint64_t hz = CPUDetect::GetInfo().frequency.tsc_frequency;
        g_tsc_per_us = hz ? (hz / 1000000ULL) : 1000;   // assume ~1ghz if unknown (satoru)
        if (g_tsc_per_us == 0) g_tsc_per_us = 1000;
    }
}
static inline uint64_t tsc_to_ns(uint64_t cycles) {
    if (g_tsc_per_us == 0) timing_prewarm();
    return (cycles * 1000ULL) / g_tsc_per_us;
}

}  // namespace

// ── store / retrieve primitives (the pool round-trip core) ──────────────────
// compress `src` (4kb) and store the blob + a metadata entry keyed (as,vaddr).
// returns the slot index, or -1 on failure (table/pool full). crc32 of the
// ORIGINAL page is recorded for verify-on-retrieve. internal; CompressPage()
// and the self-test call it. caller holds g_lock. (satoru)
static int store_compressed_locked(uint64_t as, uint64_t vaddr,
                                    const uint8_t* src, uint32_t pte_flags) {
    uint32_t crc = KMemXLZ4::Crc32(src, PAGE);
    int csz = KMemXLZ4::Compress(src, PAGE, g_scratch_comp,
                                 KMemXLZ4::CompressBound(PAGE), g_scratch_hash);
    uint16_t flags = KMETA_NONE;
    const uint8_t* blob;
    uint32_t blob_len;
    if (csz <= 0 || csz >= PAGE) {
        // incompressible (or expanded): store the raw page. (satoru)
        flags = KMETA_RAW;
        blob = src;
        blob_len = PAGE;
        g_stats.compress_fail++;
    } else {
        blob = g_scratch_comp;
        blob_len = (uint32_t)csz;
    }

    uint32_t off = KMemXPool::Alloc(blob_len);
    if (off == KMEMX_POOL_NULL) return -1;   // pool full (satoru)

    int slot = meta_insert(as, vaddr);
    if (slot < 0) { KMemXPool::Free(off, blob_len); return -1; }   // table full (satoru)

    void* dst = KMemXPool::Ptr(off);
    memcpy(dst, blob, blob_len);

    PageMeta& m = g_meta[slot];
    m.pool_off = off;
    m.comp_size = (uint16_t)blob_len;
    m.flags = flags;
    m.crc32 = crc;
    m.orig_pte_flags = pte_flags;
    m.dedup_refs = 1;
    m.generation = 0;
    m.lock_bit = 0;

    g_meta_live++;
    g_stats.pages_in++;
    g_stats.live_pages = g_meta_live;
    g_stats.pool_used = KMemXPool::UsedBytes();
    return slot;
}

// retrieve the page for slot `idx` into `dst` (4kb), verifying crc32. returns
// true on success; a crc mismatch returns false AND the caller must panic
// (silent memory corruption). does NOT free the slot. caller holds g_lock. (satoru)
static bool retrieve_compressed_locked(int idx, uint8_t* dst) {
    PageMeta& m = g_meta[idx];
    void* src = KMemXPool::Ptr(m.pool_off);
    if (!src) return false;
    if (m.flags & KMETA_RAW) {
        memcpy(dst, src, PAGE);
    } else {
        int dsz = KMemXLZ4::Decompress((const uint8_t*)src, m.comp_size, dst, PAGE);
        if (dsz != PAGE) return false;   // malformed blob (satoru)
    }
    uint32_t crc = KMemXLZ4::Crc32(dst, PAGE);
    if (crc != m.crc32) {
        g_stats.panics_avoided++;        // a crc mismatch is fatal; counted for diag (satoru)
        return false;
    }
    return true;
}

// free slot `idx`'s pool extent + metadata. caller holds g_lock. dedup-aware: a
// shared extent (KMETA_DEDUP) is only returned to the pool when its last
// reference is freed; until then we just drop this page's metadata + refcount.
// (satoru)
static void free_slot_locked(int idx) {
    PageMeta& m = g_meta[idx];
    if (m.pool_off == KMEMX_POOL_NULL) return;

    if (m.flags & KMETA_DEDUP) {
        // a deduped page: decrement the shared extent's refcount and free the pool
        // bytes only when the LAST reference goes (refs reaches 0). the refcount
        // == the number of metas pointing at this extent, so the entry stays live
        // (even at refs==1) until its final page is freed. (satoru)
        int d = dedup_find(m.pool_off);
        if (d >= 0) {
            if (g_dedup[d].refs > 0) g_dedup[d].refs--;
            if (g_dedup[d].refs == 0) {
                KMemXPool::Free(m.pool_off, m.comp_size);
                g_stats.pool_used = KMemXPool::UsedBytes();
                g_dedup[d].pool_off = 0;
                g_dedup_count--;
            }
            // every freed deduped page (except the one keeping the extent) gives
            // back its saved bytes; track the live dedup-eliminated count. (satoru)
            if (g_stats.dedup_saved > 0) g_stats.dedup_saved--;
        }
        meta_erase(idx);
        g_meta_live--;
        g_stats.live_pages = g_meta_live;
        return;
    }

    KMemXPool::Free(m.pool_off, m.comp_size);
    meta_erase(idx);
    g_meta_live--;
    g_stats.live_pages = g_meta_live;
    g_stats.pool_used = KMemXPool::UsedBytes();
}

// ── public lifecycle ─────────────────────────────────────────────────────────
// the table is capped so it never needs a giant contiguous allocation, and the
// pool is sized against *current free* memory with headroom so it can never
// starve the heap/buddy/the rest of the kernel of contiguous frames. (satoru)
constexpr uint32_t META_MAX_ENTRIES = 256 * 1024;   // 256k entries (~10mb @ 40b) (satoru)
constexpr uint64_t POOL_HEADROOM    = 256ULL * 1024 * 1024;  // leave >=256mb free (satoru)

bool Init(int pool_pct) {
    if (g_inited) return true;
    if (pool_pct < 10) pool_pct = 10;
    if (pool_pct > 40) pool_pct = 40;
    g_pool_pct = pool_pct;

    for (int i = 0; i < (int)(sizeof(Stats) / 8); i++) ((uint64_t*)&g_stats)[i] = 0;

    // ── 1) scratch + metadata FIRST, while contiguous memory is plentiful ──
    // (allocating these after the pool grabbed hundreds of 2mb chunks left no
    //  contiguous block for the table - the original boot failure.) (satoru)
    g_scratch_hash = (uint8_t*)PMM::AllocBytes(KMemXLZ4::SCRATCH_BYTES);
    g_scratch_comp = (uint8_t*)PMM::AllocBytes((size_t)KMemXLZ4::CompressBound(PAGE));
    g_scratch_dda  = (uint8_t*)PMM::AllocBytes(PAGE);   // dedup byte-compare A (satoru)
    g_scratch_ddb  = (uint8_t*)PMM::AllocBytes(PAGE);   // dedup byte-compare B (satoru)
    if (!g_scratch_hash || !g_scratch_comp || !g_scratch_dda || !g_scratch_ddb) {
        SerialLogger::Log("[kmemx] FATAL: could not allocate scratch\r\n");
        return false;
    }

    // metadata table: ~1 entry per 1.5kb of the INTENDED pool, power-of-two,
    // hard-capped at META_MAX_ENTRIES so the contiguous allocation stays small
    // (a larger pool than the cap can serve just limits concurrent live pages - 
    // a soft cap, never a crash). (satoru)
    uint64_t total_ram = PMM::GetTotalMemory();
    uint64_t intended_pool = (total_ram / 100) * (uint64_t)pool_pct;
    uint32_t entries = (uint32_t)(intended_pool / 1500);
    if (entries < 1024) entries = 1024;
    if (entries > META_MAX_ENTRIES) entries = META_MAX_ENTRIES;
    g_meta_cap = next_pow2(entries);
    g_meta_mask = g_meta_cap - 1;
    g_meta = (PageMeta*)PMM::AllocBytes((size_t)g_meta_cap * sizeof(PageMeta));
    if (!g_meta) {
        SerialLogger::Log("[kmemx] FATAL: could not allocate metadata table\r\n");
        return false;
    }
    for (uint32_t i = 0; i < g_meta_cap; i++) {
        g_meta[i].address_space = 0;
        g_meta[i].vaddr = 0;
        g_meta[i].pool_off = KMEMX_POOL_NULL;
    }

    // ── 2) reserve the pool LAST, bounded so it leaves headroom ──
    // never reserve so much that fewer than POOL_HEADROOM bytes stay free, and
    // never exceed what the metadata table can index (entries * ~max-blob). the
    // chunk loop also stops on the first failed contiguous alloc, so on a tight
    // host we keep whatever chunks we got rather than starving the kernel. (satoru)
    uint64_t free_bytes = PMM::GetFreeMemory();
    uint64_t want = intended_pool;
    if (free_bytes > POOL_HEADROOM) {
        uint64_t max_safe = free_bytes - POOL_HEADROOM;
        if (want > max_safe) want = max_safe;
    } else {
        want = 0;   // memory too tight to compress safely (satoru)
    }
    // and never take more than HALF of currently-free ram, so the heap + apps +
    // guests have ample room to grow afterwards without immediately driving the
    // pressure system to red/critical. (the pmm can over-report total ram on some
    // hosts, so sizing off free + this proportional cap is the safe rule.) (satoru)
    uint64_t half_free = free_bytes / 2;
    if (want > half_free) want = half_free;
    // also bound by table capacity (avg blob ~2kb -> entries*2kb of pool is
    // the most we could ever fill). (satoru)
    uint64_t table_bound = (uint64_t)g_meta_cap * 2048ULL;
    if (want > table_bound) want = table_bound;

    uint64_t got = (want > 0) ? KMemXPool::Reserve(want) : 0;
    if (got == 0) {
        SerialLogger::Log("[kmemx] WARN: reserved 0 pool (low memory) - engine idle\r\n");
        // not fatal: the engine inits but compresses nothing until memory frees
        // up and SetPoolPct grows it. (satoru)
    }
    g_stats.pool_bytes = got;

    // never-compress: kmemx's own pool + metadata + scratch (do not compress the
    // thing that holds the compressed pages!). the rest of the never-list is
    // registered by the owning subsystems via ReserveNeverCompress. (satoru)
    ReserveNeverCompress((uint64_t)(uintptr_t)g_meta,
                         (uint64_t)g_meta_cap * sizeof(PageMeta), "kmemx.meta");
    ReserveNeverCompress((uint64_t)(uintptr_t)g_scratch_hash, KMemXLZ4::SCRATCH_BYTES, "kmemx.scratch");
    ReserveNeverCompress((uint64_t)(uintptr_t)g_scratch_comp, (uint64_t)KMemXLZ4::CompressBound(PAGE), "kmemx.scratch2");
    ReserveNeverCompress((uint64_t)(uintptr_t)g_scratch_dda, PAGE, "kmemx.dedupA");
    ReserveNeverCompress((uint64_t)(uintptr_t)g_scratch_ddb, PAGE, "kmemx.dedupB");

    // pre-warm the fault hot path: build the crc32 table + cache the tsc freq now
    // (one-time costs that would otherwise land on the FIRST decompression fault
    // and blow its latency). a single crc over the scratch buffer builds the
    // table; timing_prewarm caches the frequency. (satoru)
    (void)KMemXLZ4::Crc32(g_scratch_comp, 64);
    timing_prewarm();

    g_inited = true;
    SerialLogger::Log("[kmemx] initialized: pool=");
    SerialLogger::LogDec((int)(got / (1024 * 1024)));
    SerialLogger::Log("MB meta_entries=");
    SerialLogger::LogDec((int)g_meta_cap);
    SerialLogger::Log("\r\n");
    return true;
}

bool IsInitialized() { return g_inited; }
bool IsEnabled()     { return g_enabled; }
void SetEnabled(bool on) { g_enabled = on; }

// ── never-compress list ──────────────────────────────────────────────────────
void ReserveNeverCompress(uint64_t phys_base, uint64_t bytes, const char* who) {
    if (g_never_count >= MAX_NEVER || bytes == 0) return;
    uint64_t f; g_lock.LockIrqSave(&f);
    g_never[g_never_count].base = phys_base & ~0xFFFULL;
    g_never[g_never_count].end  = (phys_base + bytes + 0xFFFULL) & ~0xFFFULL;
    g_never[g_never_count].who  = who;
    g_never_count++;
    g_lock.UnlockIrqRestore(f);
    SerialLogger::Log("[kmemx] never-compress: ");
    SerialLogger::Log(who ? who : "?");
    SerialLogger::Log("\r\n");
}

bool IsCompressible(uint64_t as, uint64_t vaddr) {
    if (!g_inited || !g_enabled) return false;
    vaddr &= ~0xFFFULL;
    // resolve the backing frame; an unmapped / huge / not-present page is not a
    // candidate (we only ever compress 4kb leaf pages). (satoru)
    uint64_t phys = KernelVMM::QueryMappingInAddressSpace(as, vaddr);
    if (phys == 0) return false;
    phys &= ~0xFFFULL;
    // never-compress physical ranges (dma rings, framebuffer, ept, our pool). (satoru)
    for (int i = 0; i < g_never_count; i++) {
        if (phys >= g_never[i].base && phys < g_never[i].end) return false;
    }
    // already compressed? (a not-present pte would already have failed the query,
    // but guard anyway against a double-take.) (satoru)
    if (meta_find(as, vaddr) >= 0) return false;
    return true;
}

// ── stats ─────────────────────────────────────────────────────────────────────
const Stats& GetStats() {
    // bytes_saved is derived exactly from the live logical size minus the real
    // physical pool bytes in use (the pool allocator tracks UsedBytes() exactly),
    // so dedup + raw-stored pages are all accounted without per-op delta bugs.
    // (satoru)
    uint64_t logical = (uint64_t)g_meta_live * PAGE;
    uint64_t used = KMemXPool::UsedBytes();
    g_stats.pool_used = used;
    g_stats.bytes_saved = (logical > used) ? (logical - used) : 0;
    return g_stats;
}

int RatioX100() {
    // ratio over live pages: (live*4096) / pool_used. (satoru)
    uint64_t used = g_stats.pool_used;
    if (used == 0) return 100;
    uint64_t logical = (uint64_t)g_meta_live * PAGE;
    return (int)((logical * 100) / used);
}

int PoolPct() { return g_pool_pct; }
int Threshold() {
    // pressure tightens the threshold (compress more aggressively). (satoru)
    switch (g_pressure) {
        case PRESS_GREEN:    return g_threshold;
        case PRESS_YELLOW:   return 6;
        case PRESS_ORANGE:   return 4;
        case PRESS_RED:      return 2;
        case PRESS_CRITICAL: return 1;
    }
    return g_threshold;
}
void SetThreshold(int gen) {
    if (gen < 4) gen = 4;
    if (gen > 16) gen = 16;
    g_threshold = gen;
}

Pressure CurrentPressure() { return g_pressure; }
const char* PressureName(Pressure p) {
    switch (p) {
        case PRESS_GREEN:    return "green";
        case PRESS_YELLOW:   return "yellow";
        case PRESS_ORANGE:   return "orange";
        case PRESS_RED:      return "red";
        case PRESS_CRITICAL: return "critical";
    }
    return "?";
}

Pressure UpdatePressure() {
    uint64_t total = PMM::GetTotalFrames();
    uint64_t free  = PMM::GetFreeFrames();
    if (total == 0) { g_pressure = PRESS_GREEN; return g_pressure; }
    int free_pct = (int)((free * 100) / total);
    if      (free_pct > 50) g_pressure = PRESS_GREEN;
    else if (free_pct > 30) g_pressure = PRESS_YELLOW;
    else if (free_pct > 15) g_pressure = PRESS_ORANGE;
    else if (free_pct > 5)  g_pressure = PRESS_RED;
    else                    g_pressure = PRESS_CRITICAL;
    return g_pressure;
}

// kinit's once-per-second pressure monitor. recompute the level; on a TRANSITION
// log it; at Orange+ aggressively compress guest VM pages (signal them to release
// memory). the returned level drives kinit's service-launch / shed decisions. (satoru)
Pressure PressureTick() {
    static Pressure last = PRESS_GREEN;
    Pressure p = UpdatePressure();
    if (p != last) {
        SerialLogger::Log("[kmemx] memory pressure -> ");
        SerialLogger::Log(PressureName(p));
        SerialLogger::Log("\r\n");
        last = p;
    }
    if (g_enabled && p >= PRESS_ORANGE) {
        // orange+: compress idle guest pages so guests give memory back. bounded
        // so this 1hz call never spikes cpu. (satoru)
        CompressGuests(64);
    }
    return p;
}

bool ShouldBlockNewServices() {
    // red / critical: stop launching new services to avoid pushing into oom. (satoru)
    return g_enabled && g_pressure >= PRESS_RED;
}

bool ShouldShedServices() {
    // critical: kinit should gracefully terminate its lowest-priority services. (satoru)
    return g_enabled && g_pressure >= PRESS_CRITICAL;
}

bool SetPoolPct(int pct) {
    if (pct < 10) pct = 10;
    if (pct > 40) pct = 40;
    g_pool_pct = pct;
    // bound the target exactly like Init: never grow so far that free ram drops
    // below POOL_HEADROOM, and never past what the metadata table can index.
    // GrowTo only ever ADDS chunks (it never shrinks), and it stops on the first
    // failed contiguous alloc, so this can only ever cap the growth - on a tight
    // host (e.g. the 2gb headless test where the pmm reports 4gb) it keeps the
    // pool from pushing the system into oom/critical. (satoru)
    uint64_t total_ram = PMM::GetTotalMemory();
    uint64_t want = (total_ram / 100) * (uint64_t)pct;
    // include what we already hold so "free" reflects post-reservation memory. (satoru)
    uint64_t free_plus_pool = PMM::GetFreeMemory() + KMemXPool::TotalBytes();
    if (free_plus_pool > POOL_HEADROOM) {
        uint64_t max_safe = free_plus_pool - POOL_HEADROOM;
        if (want > max_safe) want = max_safe;
    } else {
        want = KMemXPool::TotalBytes();   // too tight to grow further (satoru)
    }
    // never more than half of (free + what we hold) - same proportional cap as
    // Init, so growing the pool can't drive the system into critical. (satoru)
    uint64_t half = free_plus_pool / 2;
    if (want > half) want = half;
    uint64_t table_bound = (uint64_t)g_meta_cap * 2048ULL;
    if (want > table_bound) want = table_bound;
    uint64_t got = KMemXPool::GrowTo(want);
    g_stats.pool_bytes = got;
    return true;
}

void RegisterGuest(uint64_t ept_root, const char* name) {
    if (g_guest_count >= MAX_GUESTS) return;
    g_guests[g_guest_count].ept_root = ept_root;
    g_guests[g_guest_count].name = name;
    g_guest_count++;
}
void UnregisterGuest(uint64_t ept_root) {
    for (int i = 0; i < g_guest_count; i++) {
        if (g_guests[i].ept_root == ept_root) {
            g_guests[i] = g_guests[--g_guest_count];
            return;
        }
    }
}

// ── stage 9: hypervisor guest-page compression (ept level) ──────────────────
// the guest has zero knowledge: we compress its host-backing frame, clear the
// ept leaf (so the next guest access faults with an ept violation), and free the
// frame - exactly the native flow but at the ept layer. the page is keyed
// (ept_root, guest_phys) in the SAME pool + metadata table, tagged KMETA_GUEST.
// only registered guests are touched; with no running guest this is inert. the
// scan cursor round-robins guest-physical space across the mapped regions. (satoru)
namespace { uint64_t g_guest_scan_gpa = 0; int g_guest_scan_idx = 0; }

// compress one guest-physical page; returns true if taken. caller must NOT hold
// g_lock (this takes it). (satoru)
static bool compress_guest_page(uint64_t ept_root, uint64_t gpa) {
    if (ept_root == 0) return false;
    gpa &= ~0xFFFULL;
    EPT_PML4* pml4 = (EPT_PML4*)(uintptr_t)ept_root;
    uint64_t* leaf = EPTManager::KmemxLeafEntry(pml4, gpa);
    if (!leaf) return false;
    uint64_t e = *leaf;
    // a present, writable, 4kb ram leaf is a candidate. skip not-present / pure
    // mmio (uncacheable) / already-compressed. (satoru)
    if (!(e & (EPT_READ | EPT_WRITE | EPT_EXECUTE))) return false;
    if (e & EPT_LARGE_PAGE) return false;
    uint64_t host_phys = e & EPT_ADDR_MASK;
    if (host_phys == 0) return false;
    // never compress an mmio-typed page (e.g. a passthrough bar). (satoru)
    if ((e & EPT_MEM_TYPE_MASK) == EPT_MT_UC) return false;
    // never-compress physical ranges still apply (e.g. ept structures). (satoru)
    for (int i = 0; i < g_never_count; i++)
        if (host_phys >= g_never[i].base && host_phys < g_never[i].end) return false;

    const uint8_t* page = (const uint8_t*)(uintptr_t)host_phys;   // identity-mapped (satoru)
    uint64_t f; g_lock.LockIrqSave(&f);
    if (meta_find(ept_root, gpa) >= 0) { g_lock.UnlockIrqRestore(f); return false; }
    int slot = store_compressed_locked(ept_root, gpa, page, 0);
    if (slot < 0) { g_lock.UnlockIrqRestore(f); return false; }
    // record the ept permission/memory-type bits to restore, and tag as guest. (satoru)
    g_meta[slot].orig_pte_flags = (uint32_t)(e & (EPT_READ | EPT_WRITE | EPT_EXECUTE |
                                                  EPT_MEM_TYPE_MASK | EPT_IGNORE_PAT));
    g_meta[slot].flags |= KMETA_GUEST;
    // clear the ept leaf -> the next guest access ept-violations. (satoru)
    *leaf = 0;
    g_lock.UnlockIrqRestore(f);

    PMM::FreeFrame(host_phys);
    EPTManager::InvalidateEPT();
    return true;
}

int CompressGuests(int budget) {
    if (!g_inited || !g_enabled || budget <= 0) return 0;
    if (g_guest_count == 0) return 0;   // no registered guest -> nothing to do (satoru)
    int taken = 0;
    int regions = EPTManager::GetRegionCount();
    if (regions <= 0) return 0;

    // walk guest-physical pages across the mapped ram regions, round-robined by a
    // persistent cursor, for the FIRST registered guest's ept root. (one guest at
    // a time keeps each pass bounded; the cursor advances guests over passes.) (satoru)
    if (g_guest_scan_idx >= g_guest_count) g_guest_scan_idx = 0;
    uint64_t ept_root = g_guests[g_guest_scan_idx].ept_root;

    const int AGE_LIMIT = budget * 64;
    int looked = 0;
    for (int r = 0; r < regions && taken < budget && looked < AGE_LIMIT; r++) {
        const GuestMemRegion* reg = EPTManager::GetRegion(r);
        if (!reg) continue;
        // only normal ram is compressible - skip rom / mmio / reserved /
        // framebuffer regions entirely (those must never be compressed). the
        // per-leaf checks in compress_guest_page are a second line of defence. (satoru)
        if (reg->type != MEM_RAM) continue;
        uint64_t base = reg->guest_phys_start;
        uint64_t end  = reg->guest_phys_start + reg->size;
        uint64_t gpa = base;
        if (g_guest_scan_gpa >= base && g_guest_scan_gpa < end) gpa = g_guest_scan_gpa & ~0xFFFULL;
        for (; gpa < end && taken < budget && looked < AGE_LIMIT; gpa += PAGE) {
            looked++;
            if (compress_guest_page(ept_root, gpa)) taken++;
        }
        g_guest_scan_gpa = gpa;
    }
    // advance to the next guest for the following pass. (satoru)
    g_guest_scan_idx++;
    return taken;
}

bool HandleGuestFault(uint64_t ept_root, uint64_t guest_phys) {
    if (!g_inited || ept_root == 0) return false;
    uint64_t gpa = guest_phys & ~0xFFFULL;

    // is this a guest page we compressed? (the metadata is keyed by ept root.) (satoru)
    uint64_t f; g_lock.LockIrqSave(&f);
    int idx = meta_find(ept_root, gpa);
    if (idx < 0 || !(g_meta[idx].flags & KMETA_GUEST)) {
        g_lock.UnlockIrqRestore(f);
        return false;   // not ours - the hypervisor handles it normally (satoru)
    }
    uint32_t ept_flags = g_meta[idx].orig_pte_flags;
    g_lock.UnlockIrqRestore(f);

    uint64_t t0 = rdtsc();
    uint64_t frame = PMM::AllocFrame();
    if (frame == 0) return false;
    uint8_t* dst = (uint8_t*)(uintptr_t)frame;

    g_lock.LockIrqSave(&f);
    idx = meta_find(ept_root, gpa);
    if (idx < 0) { g_lock.UnlockIrqRestore(f); PMM::FreeFrame(frame); return false; }
    if (!retrieve_compressed_locked(idx, dst)) {
        uint32_t want = g_meta[idx].crc32;
        g_lock.UnlockIrqRestore(f);
        KernelPanic::KeBugCheckEx(StopCode::KMEMX_CORRUPTION, ept_root, gpa,
                                  (uint64_t)want, 4,
                                  "kmemx: crc mismatch decompressing a guest page",
                                  __FILE__, (uint32_t)__LINE__);
    }
    // re-establish the ept leaf -> fresh frame with the saved r/w/x + memtype. (satoru)
    EPT_PML4* pml4 = (EPT_PML4*)(uintptr_t)ept_root;
    uint64_t* leaf = EPTManager::KmemxLeafEntry(pml4, gpa);
    if (leaf) *leaf = (frame & EPT_ADDR_MASK) | (uint64_t)ept_flags;
    free_slot_locked(idx);
    g_stats.pages_out++;
    g_stats.faults_served++;
    uint64_t dt = tsc_to_ns(rdtsc() - t0);
    if (dt > g_stats.ns_decompress_max) g_stats.ns_decompress_max = dt;
    g_lock.UnlockIrqRestore(f);

    EPTManager::InvalidateEPT();
    return true;
}

bool HandleGuestFaultAny(uint64_t guest_phys) {
    if (!g_inited || g_guest_count == 0) return false;
    // snapshot the roots under the lock (the list is tiny), then try each. (satoru)
    uint64_t roots[MAX_GUESTS]; int n;
    {
        uint64_t f; g_lock.LockIrqSave(&f);
        n = g_guest_count;
        for (int i = 0; i < n; i++) roots[i] = g_guests[i].ept_root;
        g_lock.UnlockIrqRestore(f);
    }
    for (int i = 0; i < n; i++) {
        if (HandleGuestFault(roots[i], guest_phys)) return true;
    }
    return false;
}

// ── stage 5/6: compress one page ─────────────────────────────────────────────
// resolve the page's backing frame, compress its bytes into the pool, make the
// leaf not-present + marked, then free the original frame. the original frame's
// bytes are identity-mapped so we read them directly from kernel context (no cr3
// switch needed - kmemx runs in the kernel address space but the target page
// tables + frames are all identity-mapped). (satoru)
bool CompressPage(uint64_t as, uint64_t vaddr) {
    if (!g_inited || !g_enabled) return false;
    vaddr &= ~0xFFFULL;
    if (!IsCompressible(as, vaddr)) return false;

    uint64_t phys = KernelVMM::QueryMappingInAddressSpace(as, vaddr);
    if (phys == 0) return false;
    phys &= ~0xFFFULL;
    const uint8_t* page = (const uint8_t*)(uintptr_t)phys;   // identity-mapped (satoru)

    uint64_t t0 = rdtsc();
    uint64_t f; g_lock.LockIrqSave(&f);

    // re-check under the lock: nothing must have compressed this in the gap. (satoru)
    if (meta_find(as, vaddr) >= 0) { g_lock.UnlockIrqRestore(f); return false; }

    int slot = store_compressed_locked(as, vaddr, page, 0);
    if (slot < 0) { g_lock.UnlockIrqRestore(f); return false; }   // pool/table full (satoru)

    // take the leaf out of service: capture its perms, mark not-present. on the
    // off chance the leaf vanished (raced an unmap), roll back the pool entry. (satoru)
    uint64_t cap_phys = 0, cap_flags = 0;
    if (!KernelVMM::KmemxMarkCompressed(as, vaddr, &cap_phys, &cap_flags) || cap_phys != phys) {
        free_slot_locked(slot);
        g_lock.UnlockIrqRestore(f);
        return false;
    }
    // record the real permission bits so the fault path restores them exactly. (satoru)
    g_meta[slot].orig_pte_flags = (uint32_t)cap_flags;
    g_meta[slot].generation = (uint8_t)KernelVMM::KmemxGetGeneration(as, vaddr);

    uint64_t dt = tsc_to_ns(rdtsc() - t0);
    if (dt > g_stats.ns_compress_max) g_stats.ns_compress_max = dt;
    g_lock.UnlockIrqRestore(f);

    // free the now-spare physical frame back to the pmm (the page lives in the
    // pool now) and flush the stale tlb entry. done OUTSIDE the lock - PMM has its
    // own irq guard, and the leaf is already not-present so no one can fault it
    // into a half state. (satoru)
    PMM::FreeFrame(phys);
    if (as == KernelVMM::GetCurrentAddressSpace()) KernelVMM::InvalidatePage(vaddr);
    return true;
}

// ── stage 6: the page-fault decompression path (target < 5us) ───────────────
// called from the #pf handler. if `fault_vaddr` in the CURRENT address space is
// a kmemx-compressed leaf, decompress it back into a fresh frame (crc-verified;
// a mismatch panics), restore the leaf, free the pool slot, and return true so
// the faulting instruction is retried. lock-held window is tiny (the lz4 decode
// of a 4kb page). (satoru)
bool HandleFault(uint64_t fault_vaddr) {
    if (!g_inited) return false;
    uint64_t as = KernelVMM::GetCurrentAddressSpace();
    uint64_t va = fault_vaddr & ~0xFFFULL;

    // cheap pte check first: is this even one of ours? (avoids taking the lock on
    // the overwhelmingly-common non-kmemx fault.) (satoru)
    if (!KernelVMM::KmemxIsCompressed(as, va)) return false;

    uint64_t t0 = rdtsc();

    // a fresh frame to decompress into. allocate BEFORE taking the lock (PMM has
    // its own guard); if the pmm is empty we cannot serve the fault. (satoru)
    uint64_t frame = PMM::AllocFrame();
    if (frame == 0) {
        // out of memory mid-fault: nothing safe to do but fail the fault (the
        // generic handler will then terminate the process / panic). (satoru)
        return false;
    }
    uint8_t* dst = (uint8_t*)(uintptr_t)frame;   // identity-mapped (satoru)

    uint64_t f; g_lock.LockIrqSave(&f);
    int idx = meta_find(as, va);
    if (idx < 0) {
        // the marker said compressed but the metadata is gone - treat as not-ours
        // (another path may have restored it). free the spare frame. (satoru)
        g_lock.UnlockIrqRestore(f);
        PMM::FreeFrame(frame);
        return false;
    }

    uint32_t saved_flags = g_meta[idx].orig_pte_flags;
    bool ok = retrieve_compressed_locked(idx, dst);
    if (!ok) {
        // crc32 mismatch (or malformed blob) == silent memory corruption. this is
        // the safety backstop the spec demands: never hand back wrong bytes. (satoru)
        uint32_t want_crc = g_meta[idx].crc32;
        g_lock.UnlockIrqRestore(f);
        KernelPanic::KeBugCheckEx(StopCode::KMEMX_CORRUPTION, as, va,
                                  (uint64_t)want_crc, 0,
                                  "kmemx: crc mismatch decompressing a pooled page",
                                  __FILE__, (uint32_t)__LINE__);
    }

    // restore the leaf to point at the fresh frame with the original perms, then
    // free the pool slot + metadata. (satoru)
    KernelVMM::KmemxRestoreLeaf(as, va, frame, saved_flags);
    free_slot_locked(idx);
    g_stats.pages_out++;
    g_stats.faults_served++;

    uint64_t dt = tsc_to_ns(rdtsc() - t0);
    if (dt > g_stats.ns_decompress_max) g_stats.ns_decompress_max = dt;
    if (g_stats.ns_decompress_min == 0 || dt < g_stats.ns_decompress_min)
        g_stats.ns_decompress_min = dt;
    g_stats.ns_decompress_sum += dt;
    if (dt > 10000) {
        // blew the 10us invisibility budget - count it (per spec: normal case is
        // 2-3us). do NOT log from the hot path: SerialLogger does port i/o that
        // would itself dwarf the budget and perturb the next measurement. the
        // count is surfaced by `kmemx status` / the self-test instead. (satoru)
        g_stats.decomp_over_10us++;
    }
    g_lock.UnlockIrqRestore(f);
    return true;
}

// ── stage 5: scan candidate address spaces and compress aged pages ──────────
namespace {
// per-scan cursor so we round-robin across processes + their regions without
// rescanning the same hot range every tick. (satoru)
uint32_t g_scan_pid_cursor = 0;
uint64_t g_scan_va_cursor  = 0;
}

int ScanAndCompress(int budget) {
    if (!g_inited || !g_enabled || budget <= 0) return 0;
    if (KMemXPool::TotalBytes() == 0) return 0;   // no pool reserved (satoru)
    int threshold = Threshold();
    int taken = 0;
    int aged = 0;
    const int AGE_LIMIT = budget * 64;   // bound the per-call work (satoru)

    // walk the process list. we hold no scheduler lock (the list is append-mostly
    // and we tolerate a transient miss), and we only read each Process' regions +
    // address_space, which are stable while the process lives. (satoru)
    for (Process* p = Scheduler::ready_queue; p && (taken < budget) && (aged < AGE_LIMIT); p = p->next) {
        if (!p->is_user()) continue;          // native+linux user processes are eligible (satoru)
        uint64_t as = p->address_space;
        if (as == 0) continue;

        for (int r = 0; r < PROCESS_MAX_USER_REGIONS && (taken < budget) && (aged < AGE_LIMIT); r++) {
            const UserMemoryRegion& reg = p->regions[r];
            if (!reg.active || reg.end <= reg.start) continue;
            // start at the cursor if it falls in this region, else the region top. (satoru)
            uint64_t va = reg.start;
            if (g_scan_pid_cursor == p->pid && g_scan_va_cursor >= reg.start &&
                g_scan_va_cursor < reg.end) {
                va = g_scan_va_cursor & ~0xFFFULL;
            }
            for (; va < reg.end && (taken < budget) && (aged < AGE_LIMIT); va += PAGE) {
                if (!KernelVMM::KmemxIsLeafPresent(as, va)) continue;
                aged++;
                int gen = KernelVMM::KmemxAgeLeaf(as, va);
                if (gen < 0) continue;
                if (gen >= threshold) {
                    if (CompressPage(as, va)) taken++;
                }
            }
            g_scan_pid_cursor = p->pid;
            g_scan_va_cursor  = va;
        }
    }
    // a full flush of the cleared-accessed-bit pages: a single FlushTLB is cheaper
    // than thousands of INVLPGs and the aged pages span many ranges. only needed
    // when we actually aged pages in the active address space. cheap + safe. (satoru)
    if (aged > 0) KernelVMM::FlushTLB();
    return taken;
}

// decompress live pages back into their owning address space. if `all`, every
// live entry; else only entries whose address_space == `as`. each page is
// decompressed into a fresh frame (crc-verified -> panic on mismatch) and the
// owning leaf is restored, then the pool slot is freed. used by the per-process
// decompress, DecompressAll (kmemx disable / flush), and proactive guest
// restore. returns pages restored. (satoru)
static int DecompressMatching(uint64_t as, bool all) {
    int restored = 0;
    // iterate the whole metadata table. we re-acquire the lock per page so a long
    // drain never holds it across thousands of decodes (keeps the fault path
    // responsive). (satoru)
    for (uint32_t i = 0; i < g_meta_cap; i++) {
        uint64_t f; g_lock.LockIrqSave(&f);
        PageMeta& m = g_meta[i];
        if (m.pool_off == KMEMX_POOL_NULL) { g_lock.UnlockIrqRestore(f); continue; }
        if (!all && m.address_space != as) { g_lock.UnlockIrqRestore(f); continue; }
        uint64_t page_as = m.address_space;
        uint64_t va = m.vaddr;
        uint32_t saved_flags = m.orig_pte_flags;
        g_lock.UnlockIrqRestore(f);

        // allocate a frame, then re-find under the lock (it may have been faulted
        // in + freed in the gap). (satoru)
        uint64_t frame = PMM::AllocFrame();
        if (frame == 0) break;   // out of memory; stop draining (satoru)
        uint8_t* dst = (uint8_t*)(uintptr_t)frame;

        g_lock.LockIrqSave(&f);
        int idx = meta_find(page_as, va);
        if (idx < 0) { g_lock.UnlockIrqRestore(f); PMM::FreeFrame(frame); continue; }
        if (!retrieve_compressed_locked(idx, dst)) {
            uint32_t want_crc = g_meta[idx].crc32;
            g_lock.UnlockIrqRestore(f);
            KernelPanic::KeBugCheckEx(StopCode::KMEMX_CORRUPTION, page_as, va,
                                      (uint64_t)want_crc, 1,
                                      "kmemx: crc mismatch draining a pooled page",
                                      __FILE__, (uint32_t)__LINE__);
        }
        KernelVMM::KmemxRestoreLeaf(page_as, va, frame, saved_flags);
        free_slot_locked(idx);
        g_stats.pages_out++;
        g_lock.UnlockIrqRestore(f);
        restored++;
    }
    return restored;
}

// ── stage 5: bulk per-process + global operations (shell + toggle) ──────────
// compress every eligible page of one process now (a minimized-app / idle-service
// instant candidate). pid is a scheduler pid. (satoru)
int CompressProcess(uint32_t pid) {
    if (!g_inited || !g_enabled) return 0;
    Process* p = Scheduler::FindProcessByPid(pid);
    if (!p || !p->is_user() || p->address_space == 0) return 0;
    uint64_t as = p->address_space;
    int taken = 0;
    for (int r = 0; r < PROCESS_MAX_USER_REGIONS; r++) {
        const UserMemoryRegion& reg = p->regions[r];
        if (!reg.active || reg.end <= reg.start) continue;
        for (uint64_t va = reg.start; va < reg.end; va += PAGE) {
            if (CompressPage(as, va)) taken++;
        }
    }
    return taken;
}

// decompress every live page belonging to one process, restoring its working set
// to physical ram. walks the metadata table for entries whose address_space
// matches. (satoru)
int DecompressProcess(uint32_t pid) {
    if (!g_inited) return 0;
    Process* p = Scheduler::FindProcessByPid(pid);
    if (!p || p->address_space == 0) return 0;
    return DecompressMatching(p->address_space, /*all=*/false);
}

// decompress EVERY live page (kmemx disable / flush). drains the whole pool. (satoru)
int DecompressAll() {
    if (!g_inited) return 0;
    return DecompressMatching(0, /*all=*/true);
}

// ── stage 10: ksm-style deduplication ───────────────────────────────────────
// find compressed pages with identical content and merge them onto one shared
// pool extent (copy-on-write: a later fault decompresses + restores the page and
// drops the shared refcount). matches by crc32 first (cheap), then a full
// byte-compare of the decompressed pages (crc collisions are rare but possible - 
// we must never merge non-identical pages). runs at the lowest priority inside
// the kmemx process and is strictly bounded by `budget` merges per pass. big win
// for libxul zero pages + musl pages shared across processes. (satoru)
namespace { uint32_t g_dedup_cursor = 0; }

int DedupPass(int budget) {
    if (!g_inited || !g_enabled || budget <= 0) return 0;
    if (g_meta_cap == 0) return 0;
    int merged = 0;
    // bound the work: scan at most this many table slots per pass. (satoru)
    const uint32_t SCAN = g_meta_cap;   // one full sweep, amortized by the cursor (satoru)
    uint32_t scanned = 0;

    uint64_t f; g_lock.LockIrqSave(&f);
    uint32_t i = g_dedup_cursor & g_meta_mask;
    while (scanned < SCAN && merged < budget) {
        scanned++;
        uint32_t slot_a = i;
        i = (i + 1) & g_meta_mask;

        PageMeta& a = g_meta[slot_a];
        if (a.pool_off == KMEMX_POOL_NULL) continue;
        if (a.flags & (KMETA_DEDUP | KMETA_RAW)) continue;   // skip already-deduped + raw (satoru)

        // decompress page A into buffer A (crc-verified). (satoru)
        if (!retrieve_compressed_locked((int)slot_a, g_scratch_dda)) {
            uint32_t want = a.crc32;
            g_lock.UnlockIrqRestore(f);
            KernelPanic::KeBugCheckEx(StopCode::KMEMX_CORRUPTION, a.address_space,
                                      a.vaddr, (uint64_t)want, 2,
                                      "kmemx: crc mismatch during dedup scan",
                                      __FILE__, (uint32_t)__LINE__);
        }
        uint32_t crc_a = a.crc32;

        // scan a bounded window forward for another page with the same crc. (satoru)
        const uint32_t WINDOW = 512;
        for (uint32_t w = 1; w <= WINDOW && merged < budget; w++) {
            uint32_t slot_b = (slot_a + w) & g_meta_mask;
            PageMeta& b = g_meta[slot_b];
            if (b.pool_off == KMEMX_POOL_NULL) continue;
            if (b.flags & (KMETA_DEDUP | KMETA_RAW)) continue;
            if (b.crc32 != crc_a) continue;
            if (b.pool_off == a.pool_off) continue;          // already the same blob (satoru)

            // crc matches - confirm with a full byte-compare. (satoru)
            if (!retrieve_compressed_locked((int)slot_b, g_scratch_ddb)) {
                uint32_t want = b.crc32;
                g_lock.UnlockIrqRestore(f);
                KernelPanic::KeBugCheckEx(StopCode::KMEMX_CORRUPTION, b.address_space,
                                          b.vaddr, (uint64_t)want, 3,
                                          "kmemx: crc mismatch during dedup compare",
                                          __FILE__, (uint32_t)__LINE__);
            }
            if (memcmp(g_scratch_dda, g_scratch_ddb, PAGE) != 0) continue;  // crc collision (satoru)

            // identical: merge B onto A's extent. intern A's extent as shared
            // (refs start at 1 for A), bump for B, then free B's own extent. (satoru)
            int d = dedup_intern(a.pool_off, 1);
            if (d < 0) break;   // dedup table full -> stop merging this pass (satoru)
            // free B's now-redundant pool bytes + point B at A's blob. (satoru)
            uint32_t b_size = b.comp_size;
            uint32_t b_off  = b.pool_off;
            // mark A as part of a dedup set too (so its free path decrements). (satoru)
            if (!(a.flags & KMETA_DEDUP)) a.flags |= KMETA_DEDUP;
            b.flags |= KMETA_DEDUP;
            b.pool_off = a.pool_off;
            b.comp_size = a.comp_size;
            g_dedup[d].refs++;            // now A + B reference the shared extent (satoru)
            KMemXPool::Free(b_off, b_size);
            g_stats.pool_used = KMemXPool::UsedBytes();
            g_stats.dedup_saved++;
            merged++;
        }
    }
    g_dedup_cursor = i;
    g_lock.UnlockIrqRestore(f);
    return merged;
}

// ── test-only hooks used by the stage-2 pool self-test ──────────────────────
// (declared in kmemx_internal.h so the test TU can reach the locked primitives
//  without exposing them on the public api.) (satoru)
int  TestStore(uint64_t as, uint64_t vaddr, const uint8_t* src) {
    uint64_t f; g_lock.LockIrqSave(&f);
    int slot = store_compressed_locked(as, vaddr, src, 0);
    g_lock.UnlockIrqRestore(f);
    return slot;
}
bool TestRetrieve(uint64_t as, uint64_t vaddr, uint8_t* dst) {
    uint64_t f; g_lock.LockIrqSave(&f);
    int idx = meta_find(as, vaddr);
    bool ok = (idx >= 0) && retrieve_compressed_locked(idx, dst);
    g_lock.UnlockIrqRestore(f);
    return ok;
}
void TestFree(uint64_t as, uint64_t vaddr) {
    uint64_t f; g_lock.LockIrqSave(&f);
    int idx = meta_find(as, vaddr);
    if (idx >= 0) free_slot_locked(idx);
    g_lock.UnlockIrqRestore(f);
}
uint32_t TestMetaLive() { return g_meta_live; }

}  // namespace KMemX

// end (satoru)
