#include "kmemx_pool.h"
#include "pmm.h"
#include "../drivers/serial.h"
#include "../proc/spinlock.h"

//  KMemX pool allocator - see kmemx_pool.h. (satoru)
//
//  each 2mb chunk is an independent heap managed by an intrusive singly-linked
//  free-list, sorted by address so coalescing on free is O(n) over free blocks
//  (n is tiny). a free block stores a FreeHdr at its start; an allocated block
//  carries no header (the owner tracks the size in page metadata), so the blob
//  bytes are not stolen by allocator overhead. requested sizes are rounded up to
//  GRAIN and to at least sizeof(FreeHdr) so a freed block can always hold a
//  header. (satoru)

namespace KMemXPool {

namespace {

struct FreeHdr {
    uint32_t next;   // intra-chunk byte offset of the next free block, or NIL (satoru)
    uint32_t size;   // size of this free block in bytes (satoru)
};
constexpr uint32_t NIL = 0xFFFFFFFFu;

struct Chunk {
    uint64_t base;        // identity-mapped physical/virtual base of the 2mb chunk (satoru)
    uint32_t free_head;   // intra-chunk offset of the first free block, or NIL (satoru)
    uint32_t free_bytes;  // total free bytes in this chunk (satoru)
};

Chunk    g_chunks[MAX_CHUNKS];
uint32_t g_chunk_count = 0;
uint64_t g_total_bytes = 0;
uint64_t g_used_bytes  = 0;
Spinlock g_lock;

constexpr uint32_t MIN_BLOCK = (uint32_t)sizeof(FreeHdr);   // 8 bytes (satoru)

static inline uint32_t round_up(uint32_t n) {
    if (n < MIN_BLOCK) n = MIN_BLOCK;
    return (n + (GRAIN - 1)) & ~(GRAIN - 1);
}

static inline FreeHdr* hdr_at(const Chunk& c, uint32_t intra) {
    return (FreeHdr*)(uintptr_t)(c.base + intra);
}

// initialise a fresh chunk as one big free block. (satoru)
static void chunk_init(Chunk& c, uint64_t base) {
    c.base = base;
    c.free_head = 0;
    c.free_bytes = (uint32_t)CHUNK_BYTES;
    FreeHdr* h = hdr_at(c, 0);
    h->next = NIL;
    h->size = (uint32_t)CHUNK_BYTES;
}

}  // namespace

uint64_t Reserve(uint64_t want_bytes) {
    uint64_t f;
    g_lock.LockIrqSave(&f);
    if (g_chunk_count == 0) {
        // first reservation - allocate chunks. (satoru)
        uint32_t want_chunks = (uint32_t)((want_bytes + CHUNK_BYTES - 1) / CHUNK_BYTES);
        if (want_chunks == 0) want_chunks = 1;
        if (want_chunks > MAX_CHUNKS) want_chunks = MAX_CHUNKS;
        for (uint32_t i = 0; i < want_chunks; i++) {
            // 512 frames == 2mb, contiguous + identity-mapped. (satoru)
            uint64_t base = PMM::AllocContiguous(CHUNK_BYTES / 4096);
            if (base == 0) break;   // pmm exhausted; keep what we got (satoru)
            chunk_init(g_chunks[g_chunk_count], base);
            g_chunk_count++;
            g_total_bytes += CHUNK_BYTES;
        }
        SerialLogger::Log("[kmemx-pool] reserved chunks=");
        SerialLogger::LogDec((int)g_chunk_count);
        SerialLogger::Log(" bytes=");
        SerialLogger::LogDec((int)(g_total_bytes / (1024 * 1024)));
        SerialLogger::Log("MB\r\n");
    }
    uint64_t total = g_total_bytes;
    g_lock.UnlockIrqRestore(f);
    return total;
}

uint64_t GrowTo(uint64_t want_bytes) {
    uint64_t f;
    g_lock.LockIrqSave(&f);
    uint32_t want_chunks = (uint32_t)((want_bytes + CHUNK_BYTES - 1) / CHUNK_BYTES);
    if (want_chunks > MAX_CHUNKS) want_chunks = MAX_CHUNKS;
    while (g_chunk_count < want_chunks) {
        uint64_t base = PMM::AllocContiguous(CHUNK_BYTES / 4096);
        if (base == 0) break;
        chunk_init(g_chunks[g_chunk_count], base);
        g_chunk_count++;
        g_total_bytes += CHUNK_BYTES;
    }
    uint64_t total = g_total_bytes;
    g_lock.UnlockIrqRestore(f);
    return total;
}

uint64_t TotalBytes() { return g_total_bytes; }
uint64_t UsedBytes()  { return g_used_bytes; }
uint32_t ChunkCount() { return g_chunk_count; }

// best-fit within one chunk: walk its free-list, pick the smallest block that
// fits, split the remainder back into the list. (satoru)
static uint32_t alloc_in_chunk(Chunk& c, uint32_t chunk_idx, uint32_t need) {
    uint32_t prev = NIL, cur = c.free_head;
    uint32_t best = NIL, best_prev = NIL, best_size = 0xFFFFFFFFu;
    while (cur != NIL) {
        FreeHdr* h = hdr_at(c, cur);
        if (h->size >= need && h->size < best_size) {
            best = cur; best_prev = prev; best_size = h->size;
            if (h->size == need) break;   // perfect fit (satoru)
        }
        prev = cur; cur = h->next;
    }
    if (best == NIL) return KMEMX_POOL_NULL;

    FreeHdr* bh = hdr_at(c, best);
    uint32_t bnext = bh->next;
    uint32_t remainder = bh->size - need;
    if (remainder >= MIN_BLOCK) {
        // split: the tail stays free. (satoru)
        uint32_t tail = best + need;
        FreeHdr* th = hdr_at(c, tail);
        th->size = remainder;
        th->next = bnext;
        if (best_prev == NIL) c.free_head = tail; else hdr_at(c, best_prev)->next = tail;
    } else {
        // consume the whole block (the slack < a header stays attached). (satoru)
        need = bh->size;
        if (best_prev == NIL) c.free_head = bnext; else hdr_at(c, best_prev)->next = bnext;
    }
    c.free_bytes -= need;
    g_used_bytes += need;
    return chunk_idx * (uint32_t)CHUNK_BYTES + best;
}

uint32_t Alloc(uint32_t n) {
    if (n == 0) return KMEMX_POOL_NULL;
    uint32_t need = round_up(n);
    if (need > CHUNK_BYTES) return KMEMX_POOL_NULL;   // never spans a chunk (satoru)

    uint64_t f;
    g_lock.LockIrqSave(&f);
    // first-chunk-that-fits, then best-fit inside it. spreads load + keeps the
    // search bounded. (satoru)
    for (uint32_t i = 0; i < g_chunk_count; i++) {
        if (g_chunks[i].free_bytes < need) continue;
        uint32_t off = alloc_in_chunk(g_chunks[i], i, need);
        if (off != KMEMX_POOL_NULL) { g_lock.UnlockIrqRestore(f); return off; }
    }
    g_lock.UnlockIrqRestore(f);
    return KMEMX_POOL_NULL;
}

void Free(uint32_t pool_off, uint32_t n) {
    if (pool_off == KMEMX_POOL_NULL) return;
    uint32_t need = round_up(n);

    uint64_t f;
    g_lock.LockIrqSave(&f);
    uint32_t chunk_idx = pool_off / (uint32_t)CHUNK_BYTES;
    uint32_t intra     = pool_off % (uint32_t)CHUNK_BYTES;
    if (chunk_idx >= g_chunk_count) { g_lock.UnlockIrqRestore(f); return; }
    Chunk& c = g_chunks[chunk_idx];

    // the actual block size: if Alloc consumed a whole block (slack < header),
    // `need` may understate it, but coalescing tolerates that - we always
    // reinsert exactly `need` and let neighbour-merge absorb adjacency. to stay
    // exact, clamp need so the block never overruns the chunk. (satoru)
    if (intra + need > CHUNK_BYTES) need = (uint32_t)CHUNK_BYTES - intra;

    // insert into the address-sorted free-list, coalescing with neighbours. (satoru)
    uint32_t prev = NIL, cur = c.free_head;
    while (cur != NIL && cur < intra) { prev = cur; cur = hdr_at(c, cur)->next; }

    FreeHdr* nh = hdr_at(c, intra);
    nh->size = need;
    nh->next = cur;
    if (prev == NIL) c.free_head = intra; else hdr_at(c, prev)->next = intra;

    // coalesce with the next block if adjacent. (satoru)
    if (cur != NIL && intra + nh->size == cur) {
        FreeHdr* ch = hdr_at(c, cur);
        nh->size += ch->size;
        nh->next = ch->next;
    }
    // coalesce with the previous block if adjacent. (satoru)
    if (prev != NIL) {
        FreeHdr* ph = hdr_at(c, prev);
        if (prev + ph->size == intra) {
            ph->size += nh->size;
            ph->next = nh->next;
        }
    }
    c.free_bytes += need;
    g_used_bytes -= need;
    g_lock.UnlockIrqRestore(f);
}

void* Ptr(uint32_t pool_off) {
    if (pool_off == KMEMX_POOL_NULL) return nullptr;
    uint32_t chunk_idx = pool_off / (uint32_t)CHUNK_BYTES;
    uint32_t intra     = pool_off % (uint32_t)CHUNK_BYTES;
    if (chunk_idx >= g_chunk_count) return nullptr;
    return (void*)(uintptr_t)(g_chunks[chunk_idx].base + intra);
}

uint32_t LargestFree() {
    uint64_t f;
    g_lock.LockIrqSave(&f);
    uint32_t largest = 0;
    for (uint32_t i = 0; i < g_chunk_count; i++) {
        uint32_t cur = g_chunks[i].free_head;
        while (cur != NIL) {
            FreeHdr* h = hdr_at(g_chunks[i], cur);
            if (h->size > largest) largest = h->size;
            cur = h->next;
        }
    }
    g_lock.UnlockIrqRestore(f);
    return largest;
}

void ResetForTest() {
    uint64_t f;
    g_lock.LockIrqSave(&f);
    for (uint32_t i = 0; i < g_chunk_count; i++) {
        chunk_init(g_chunks[i], g_chunks[i].base);
    }
    g_used_bytes = 0;
    g_lock.UnlockIrqRestore(f);
}

}  // namespace KMemXPool

// end (satoru)
