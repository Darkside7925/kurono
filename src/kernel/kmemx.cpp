#include "kmemx.h"
#include "kmemx_internal.h"
#include "kmemx_lz4.h"
#include "kmemx_pool.h"
#include "pmm.h"
#include "vmm.h"
#include "../drivers/serial.h"
#include "../proc/spinlock.h"

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
bool      g_enabled  = false;       // user toggle (stage 11)  -  off until enabled (satoru)
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
// address_space==0 but pool_off!=NULL... we don't tombstone  -  we backfill on
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
    g_stats.bytes_saved += (PAGE - blob_len);
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

// free slot `idx`'s pool extent + metadata. caller holds g_lock. (satoru)
static void free_slot_locked(int idx) {
    PageMeta& m = g_meta[idx];
    if (m.pool_off == KMEMX_POOL_NULL) return;
    g_stats.bytes_saved -= (PAGE - m.comp_size);
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
    //  contiguous block for the table  -  the original boot failure.) (satoru)
    g_scratch_hash = (uint8_t*)PMM::AllocBytes(KMemXLZ4::SCRATCH_BYTES);
    g_scratch_comp = (uint8_t*)PMM::AllocBytes((size_t)KMemXLZ4::CompressBound(PAGE));
    if (!g_scratch_hash || !g_scratch_comp) {
        SerialLogger::Log("[kmemx] FATAL: could not allocate scratch\r\n");
        return false;
    }

    // metadata table: ~1 entry per 1.5kb of the INTENDED pool, power-of-two,
    // hard-capped at META_MAX_ENTRIES so the contiguous allocation stays small
    // (a larger pool than the cap can serve just limits concurrent live pages  - 
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
    // also bound by table capacity (avg blob ~2kb -> entries*2kb of pool is
    // the most we could ever fill). (satoru)
    uint64_t table_bound = (uint64_t)g_meta_cap * 2048ULL;
    if (want > table_bound) want = table_bound;

    uint64_t got = (want > 0) ? KMemXPool::Reserve(want) : 0;
    if (got == 0) {
        SerialLogger::Log("[kmemx] WARN: reserved 0 pool (low memory)  -  engine idle\r\n");
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
const Stats& GetStats() { return g_stats; }

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

bool SetPoolPct(int pct) {
    if (pct < 10) pct = 10;
    if (pct > 40) pct = 40;
    g_pool_pct = pct;
    uint64_t total_ram = PMM::GetTotalMemory();
    uint64_t want = (total_ram / 100) * (uint64_t)pct;
    uint64_t got = KMemXPool::GrowTo(want);     // only safe-grows for now (satoru)
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

// ── stage-2 placeholder definitions (completed in later stages) ─────────────
// these touch the page tables / scheduler and are fully implemented in stages 3
// (aging), 5 (scan/process bulk ops), 6 (fault path), 9 (guests), 10 (dedup).
// they are defined now (as safe no-ops) so the header's surface is stable and
// the engine links cleanly at every stage boundary; each gets its real body in
// its own stage's commit. (satoru)
bool CompressPage(uint64_t /*as*/, uint64_t /*vaddr*/) { return false; }   // stage 5/6 (satoru)
bool HandleFault(uint64_t /*fault_vaddr*/)             { return false; }   // stage 6 (satoru)
int  ScanAndCompress(int /*budget*/)                   { return 0; }       // stage 5 (satoru)
int  CompressProcess(uint32_t /*pid*/)                 { return 0; }       // stage 5 (satoru)
int  DecompressProcess(uint32_t /*pid*/)               { return 0; }       // stage 5 (satoru)
int  DecompressAll()                                   { return 0; }       // stage 5/11 (satoru)
int  DedupPass(int /*budget*/)                         { return 0; }       // stage 10 (satoru)

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
