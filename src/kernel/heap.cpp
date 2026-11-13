#include "heap.h"
#include "pmm.h"
#include "../drivers/serial.h"
#include "../proc/smp.h"   // cpu index for the cross-core heap lock (satoru)
#include "taskmem.h"       // kurono.taskmem=1 per-task accounting hooks (satoru)

//  kernel heap - segregated free-list allocator with coalescing
//
//  phase 1: 64 kb bootstrap buffer (in bss - always available)
//  phase 2: pmm-backed region sized to a quarter of ram, grown on demand by
//           adding more pmm regions when an allocation does not fit
//
//  Each block has a HeapBlock header followed by `size` bytes of payload.
//  Free blocks store a doubly-linked-list pointer pair in the payload
//  area so allocation is O(1) freelist-pop, free is O(1) link-in, and
//  coalesce becomes O(1) by checking neighbour blocks via address
//  arithmetic.  A footer mirroring `size` is written just before each
//  block to enable backward coalescing without scanning the whole heap.

// 64 kb bootstrap heap (in bss - zeroed by boot asm). 16-byte aligned so
// HeapBlock header / FreeLink overlay stays naturally aligned.
#define BOOT_HEAP_SIZE (64ULL * 1024)
alignas(16) uint8_t KernelHeap::boot_buffer[BOOT_HEAP_SIZE];
const size_t   KernelHeap::BOOT_CAPACITY = BOOT_HEAP_SIZE;

uint8_t*       KernelHeap::heap_base     = nullptr;
size_t         KernelHeap::heap_capacity = 0;
bool           KernelHeap::initialized   = false;
bool           KernelHeap::expanded      = false;

// in-payload links for free blocks (overlay the first 16 bytes of payload).
struct FreeLink {
    FreeLink* next;
    FreeLink* prev;
    uint64_t  list;   // FREELIST_TAG | which list it was pushed on (satoru)
};

// header is 16 bytes; minimum free-block payload must fit FreeLink (16) and
// the 8-byte trailing footer side-by-side. Round up to 16-alignment → 32.
#define HEAP_MIN_PAYLOAD  32
#define HEAP_FOOTER_SIZE  8

static FreeLink* g_free_head = nullptr;   // free blocks with payload < HEAP_BIG_BLOCK (satoru)
// two size-segregated freelists. the single first-fit list made a large request
// walk every small free block first: with the heap fragmented by firefox, a 256
// kb kvfs content grow took 89 ms and an 8 mb one 134 ms inside KernelHeap::
// Alloc, all under kls (the gui stalled ~100 ms behind those file writes). a big
// request now walks only big blocks; a small one tries small blocks, then big.
// the list a block sits on is tagged in its link (stale writers can trample a
// free payload, so an unreadable tag falls back to the validated walk). (satoru)
static FreeLink* g_free_big = nullptr;
#define HEAP_BIG_BLOCK  (64ULL * 1024)
#define FREELIST_TAG    0x4B46524545000000ULL   // "KFREE" + list bit (satoru)

// rate-limit heap diagnostic warnings. a single bad/double free that gets
// logged travels serial -> runtimelog -> kvfs, which itself churns the heap
// and can trigger MORE bad frees: an unbounded feedback loop that floods the
// log and faults during boot. capping total warnings makes a stray free a
// harmless no-op instead of a cascade. (satoru)
static int g_heap_warn_budget = 64;
static void heap_warn(const char* msg) {
    if (g_heap_warn_budget > 0) { g_heap_warn_budget--; SerialLogger::Log(msg); }
}

// Interrupt-safe critical section for every public heap operation. The heap is
// re-entered from INTERRUPT context: PIT IRQ0 -> Scheduler::OnTimerTick ->
// HRTimer::Tick fires the periodic "proc_refresh" callback inline, which calls
// KVFS::WriteString -> KernelHeap::Alloc/Free. If IRQ0 lands while a process is
// mid-Alloc/Free (e.g. between freelist_remove and mark_used, or mid-coalesce
// with g_free_head dangling), the IRQ-context allocation corrupts a block
// header -> "Free() bad magic" (the 63x-at-boot symptom; the warn->serial->kvfs
// logging path then amplifies one corruption to the saturated budget). since
// smp thread dispatch, cli alone is not enough: an ap running a user thread's
// syscall allocates concurrently with the bsp's kernel procs, so the guard is
// now cli + a CROSS-CORE lock. the lock is cpu-owner-recursive because same-cpu
// re-entry still exists and cli can't stop it: a #pf (kernel stack grow, kmemx)
// mid-Alloc re-enters the heap on the SAME core - a plain spinlock would
// self-deadlock there. saves/restores IF so nested guards don't prematurely
// re-enable. (satoru)
static volatile uint32_t g_heap_lock_word  = 0;
static volatile int      g_heap_lock_owner = -1;
struct HeapIrqGuard {
    uint64_t flags;
    bool nested;
    int cpu;   // kept for the taskmem owner lookup (satoru)
    HeapIrqGuard() {
        __asm__ __volatile__("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
        cpu = (int)SMP::CpuIndex();
        if (g_heap_lock_owner == cpu) { nested = true; return; }
        nested = false;
        for (;;) {
            uint32_t expected = 0;
            if (__atomic_compare_exchange_n(&g_heap_lock_word, &expected, 1u, false,
                                            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) break;
            do { __asm__ __volatile__("pause" ::: "memory"); }
            while (__atomic_load_n(&g_heap_lock_word, __ATOMIC_RELAXED) != 0);
        }
        g_heap_lock_owner = cpu;
    }
    ~HeapIrqGuard() {
        if (!nested) {
            g_heap_lock_owner = -1;
            __atomic_store_n(&g_heap_lock_word, 0u, __ATOMIC_RELEASE);
        }
        if (flags & 0x200ULL) __asm__ __volatile__("sti" ::: "memory");
    }
};

static inline uint64_t* footer_of(HeapBlock* b) {
    return (uint64_t*)((uint8_t*)b + HEAP_HEADER_SIZE + b->size - HEAP_FOOTER_SIZE);
}

static inline bool block_used(const HeapBlock* b) {
    return (b->flags & HEAP_BLOCK_USED) != 0;
}

static inline bool valid_magic(uint64_t flags) {
    uint64_t m = flags & ~HEAP_BLOCK_USED;
    return m == (HEAP_MAGIC_FREE & ~HEAP_BLOCK_USED) ||
           m == (HEAP_MAGIC_USED & ~HEAP_BLOCK_USED);
}

// freelist link validation (linux SLAB_FREELIST_HARDENED / CONFIG_DEBUG_LIST
// analogue). a FreeLink lives in a FREE block's payload; the documented stale-
// writer class (a use-after-free write - "path buffers observed landing in
// recycled blocks") can overwrite that payload with arbitrary data, and the raw
// pointer walks below then dereference/store through garbage - observed as the
// rip=0x3 #UD boot crash with ascii string bytes in the registers. validate
// every link BEFORE it is dereferenced: in-bounds of a heap region, aligned,
// header carries the free magic, size sane. on failure the caller logs and
// truncates/leaks instead of walking garbage. bounds mirror heap_base/
// heap_capacity because these helpers are file-static. (satoru)
static uint8_t* g_val_boot = nullptr;
static size_t   g_val_boot_cap = 0;

// active heap regions. before expansion the only region is the bootstrap
// buffer; ExpandWithPMM installs region 0 and heap_grow appends more when an
// allocation does not fit. blocks never span regions, so coalescing and
// link validation are bounded by the region a block lives in. (satoru)
#define HEAP_MAX_REGIONS 32
static uint8_t* g_reg_base[HEAP_MAX_REGIONS];
static size_t   g_reg_cap[HEAP_MAX_REGIONS];
static int      g_reg_count = 0;
static size_t   g_total_cap = 0;
static bool     g_growing   = false;   // blocks re-entrant growth from a nested alloc

static int region_of(const uint8_t* p) {
    for (int i = 0; i < g_reg_count; i++)
        if (p >= g_reg_base[i] && p < g_reg_base[i] + g_reg_cap[i]) return i;
    return -1;
}

static bool link_ok(FreeLink* l) {
    uint8_t* p = (uint8_t*)l;
    int r = region_of(p);
    bool in_active = r >= 0 && p >= g_reg_base[r] + HEAP_HEADER_SIZE &&
                     p + sizeof(FreeLink) <= g_reg_base[r] + g_reg_cap[r];
    bool in_boot   = g_val_boot && p >= g_val_boot + HEAP_HEADER_SIZE &&
                     p + sizeof(FreeLink) <= g_val_boot + g_val_boot_cap;
    if (!in_active && !in_boot) return false;
    if (((uintptr_t)p & 15) != 0) return false;   // blocks + payloads are 16-aligned (satoru)
    HeapBlock* b = (HeapBlock*)(p - HEAP_HEADER_SIZE);
    if (!valid_magic(b->flags) || block_used(b)) return false;
    uint64_t cap = in_active ? (uint64_t)g_reg_cap[r] : (uint64_t)g_val_boot_cap;
    if (b->size < HEAP_MIN_PAYLOAD || b->size > cap) return false;
    return true;
}

static FreeLink** freelist_head(uint64_t which) { return which ? &g_free_big : &g_free_head; }

static void freelist_push(HeapBlock* b) {
    FreeLink* l = (FreeLink*)((uint8_t*)b + HEAP_HEADER_SIZE);
    uint64_t which = b->size >= HEAP_BIG_BLOCK ? 1 : 0;
    FreeLink** head = freelist_head(which);
    l->prev = nullptr;
    l->list = FREELIST_TAG | which;
    l->next = *head;
    if (*head) (*head)->prev = l;
    *head = l;
}

// the validated walk of one list, unlinking l if found. a corrupt entry truncates
// the list there (leak, contained). returns true when l was found. (satoru)
static bool freelist_heal_remove(FreeLink** head, FreeLink* l) {
    FreeLink* prev = nullptr;
    for (FreeLink* it = *head; it; it = it->next) {
        if (!link_ok(it)) {   // corrupt tail: truncate the list here (leak) (satoru)
            if (prev) prev->next = nullptr; else *head = nullptr;
            return false;
        }
        if (it == l) {
            FreeLink* nx = (l->next && link_ok(l->next)) ? l->next : nullptr;
            if (prev) prev->next = nx; else *head = nx;
            if (nx) nx->prev = prev;
            return true;
        }
        prev = it;
    }
    return false;
}

static void freelist_remove(HeapBlock* b) {
    FreeLink* l = (FreeLink*)((uint8_t*)b + HEAP_HEADER_SIZE);
    // validate both neighbor links before storing through them - a trampled
    // payload would otherwise redirect these writes over live memory. on a bad
    // link, fall back to a validated walk from the head; entries that cannot be
    // safely unlinked are leaked (contained), never written through. (satoru)
    bool pv = (l->prev == nullptr) || link_ok(l->prev);
    bool nv = (l->next == nullptr) || link_ok(l->next);
    bool tag_ok = (l->list & ~1ULL) == FREELIST_TAG;
    if (pv && nv && tag_ok) {
        if (l->prev) l->prev->next = l->next;
        else         *freelist_head(l->list & 1) = l->next;
        if (l->next) l->next->prev = l->prev;
        l->next = l->prev = nullptr;
        return;
    }
    heap_warn("Heap: freelist corrupt link in remove (healed)\r\n");
    if (!freelist_heal_remove(&g_free_head, l)) freelist_heal_remove(&g_free_big, l);
    l->next = l->prev = nullptr;
}

static HeapBlock* block_from_link(FreeLink* l) {
    return (HeapBlock*)((uint8_t*)l - HEAP_HEADER_SIZE);
}

static void mark_free(HeapBlock* b) {
    b->flags = HEAP_MAGIC_FREE;
    *footer_of(b) = b->size;
}

static void mark_used(HeapBlock* b) {
    b->flags = HEAP_MAGIC_USED;
    *footer_of(b) = b->size | (1ULL << 63);
}

// large allocations go straight to pmm frames, each its own block with the
// normal header/footer (so Realloc, IsValidBlock and the census see an
// ordinary used block) and back to the pmm on Free. the region heap kept them
// in its arenas: a growing kvfs file reallocates geometrically, every copy
// landed in fresh space, and the reserve climbed 110 -> 550 MB over a long
// claude session while live use stayed ~75 MB, because small long-lived
// blocks pinned every region. kurono.heapdirect=0 keeps them in the regions
// (satoru)
#define HEAP_DIRECT_MIN  (256ULL * 1024)
#define HEAP_DIRECT_MAX  4096
struct DirectAlloc { uint8_t* base; size_t bytes; };
static DirectAlloc g_direct[HEAP_DIRECT_MAX];
static int         g_direct_n = 0;
static size_t      g_direct_bytes = 0;
// heap kept grabbing new regions and never giving them back. typical
bool g_heap_direct = true;
static int direct_find(const uint8_t* payload) {
    for (int i = 0; i < g_direct_n; i++)
        if (g_direct[i].base + HEAP_HEADER_SIZE == payload) return i;
    return -1;
}

static HeapBlock* prev_block(uint8_t* heap_base_ptr, HeapBlock* b) {
    if ((uint8_t*)b <= heap_base_ptr + HEAP_HEADER_SIZE) return nullptr;
    uint64_t* prev_footer = (uint64_t*)((uint8_t*)b - HEAP_FOOTER_SIZE);
    uint64_t footer = *prev_footer;
    uint64_t prev_size = footer & ~(1ULL << 63);
    if (prev_size == 0 || prev_size > 0x10000000ULL) return nullptr;
    uint64_t total_off = HEAP_HEADER_SIZE + prev_size;
    if ((uint8_t*)b < heap_base_ptr + total_off) return nullptr;
    HeapBlock* p = (HeapBlock*)((uint8_t*)b - total_off);
    if (!valid_magic(p->flags)) return nullptr;
    if (p->size != prev_size) return nullptr;
    return p;
}

static HeapBlock* next_block(uint8_t* heap_end, HeapBlock* b) {
    uint8_t* n = (uint8_t*)b + HEAP_HEADER_SIZE + b->size;
    if (n + HEAP_HEADER_SIZE > heap_end) return nullptr;
    HeapBlock* nb = (HeapBlock*)n;
    if (!valid_magic(nb->flags)) return nullptr;
    return nb;
}

void KernelHeap::Init() {
    if (initialized) return;

    heap_base     = boot_buffer;
    heap_capacity = BOOT_HEAP_SIZE;

    HeapBlock* first = (HeapBlock*)heap_base;
    first->size  = heap_capacity - HEAP_HEADER_SIZE;
    mark_free(first);
    g_free_head = nullptr;
    g_free_big = nullptr;   // (satoru)
    freelist_push(first);

    // seed the link-validation bounds (see link_ok). (satoru)
    g_val_boot = boot_buffer; g_val_boot_cap = BOOT_HEAP_SIZE;
    g_reg_base[0] = heap_base; g_reg_cap[0] = heap_capacity;
    g_reg_count = 1; g_total_cap = heap_capacity;

    initialized = true;
    SerialLogger::Log("Heap: Bootstrap (64 KB BSS)\r\n");
}

void KernelHeap::ExpandWithPMM() {
    if (expanded) return;
    if (!initialized) Init();

    uint64_t total_phys = PMM::GetTotalMemory();

    // start small and grow on demand (heap_grow). the heap used to grab a
    // quarter of ram up front (2 gb on a 32 gb machine) whether or not anything
    // ever used it; the idle desktop needs ~15 mb and firefox's closure grows
    // the arena in a few steps when it is actually launched. the half-of-ram
    // variant starved a 512 mb machine too. (satoru)
    const uint64_t MAX_HEAP = 2ULL * 1024 * 1024 * 1024;
    const uint64_t MIN_HEAP = 16ULL * 1024 * 1024;
    uint64_t target = MIN_HEAP;
    if (target > MAX_HEAP) target = MAX_HEAP;
    if (target < MIN_HEAP) target = MIN_HEAP;

    void* big = nullptr;
    while (target >= MIN_HEAP) {
        big = PMM::AllocBytes((size_t)target);
        if (big) break;
        target /= 2;
    }

    if (!big) {
        SerialLogger::Log("Heap: PMM expand FAILED - staying on 64 KB bootstrap\r\n");
        return;
    }

    uint8_t* new_base = (uint8_t*)big;
    size_t   new_cap  = (size_t)target;

    HeapBlock* first = (HeapBlock*)new_base;
    first->size  = new_cap - HEAP_HEADER_SIZE;
    mark_free(first);

    heap_base     = new_base;
    heap_capacity = new_cap;
    expanded      = true;
    g_reg_base[0] = heap_base; g_reg_cap[0] = heap_capacity;   // re-seed link validation bounds (satoru)
    g_reg_count = 1; g_total_cap = heap_capacity;

    // abandon the bootstrap freelist; allocations in bss remain valid because
    // their headers don't move. New allocations come from the expanded pool.
    g_free_head = nullptr;
    g_free_big = nullptr;   // (satoru)
    freelist_push(first);

    uint32_t mb = (uint32_t)(target / (1024 * 1024));
    SerialLogger::Log("Heap: Expanded to ");
    SerialLogger::LogDec(mb);
    SerialLogger::Log(" MB (PMM-backed, ");
    SerialLogger::LogDec((uint32_t)(total_phys / (1024*1024)));
    SerialLogger::Log(" MB physical RAM detected)\r\n");
    LogMemReport("heap-up");
}

// add a pmm region big enough for a `need`-byte block. grows geometrically
// (half the current heap, at least 16 mb) so the region count stays small, and
// falls back to just what the request needs when ram is tight. called with the
// heap lock held; the lock order heap -> pmm matches the existing heap ->
// serial warning path, and pmm never calls back into the heap. (satoru)
// install a fresh pmm chunk as a heap region (heap lock held). (satoru)
static void heap_add_region(void* mem, size_t chunk) {
    HeapBlock* first = (HeapBlock*)mem;
    first->size = chunk - HEAP_HEADER_SIZE;
    mark_free(first);
    g_reg_base[g_reg_count] = (uint8_t*)mem;
    g_reg_cap[g_reg_count]  = chunk;
    g_reg_count++;
    g_total_cap += chunk;
    freelist_push(first);
}

static bool heap_grow(size_t need) {
    if (g_growing || g_reg_count >= HEAP_MAX_REGIONS) return false;
    const size_t ALIGN = 2ULL * 1024 * 1024;
    size_t min_chunk = need + HEAP_HEADER_SIZE + HEAP_MIN_PAYLOAD;
    if (min_chunk < need) return false;
    min_chunk = (min_chunk + ALIGN - 1) & ~(ALIGN - 1);
    size_t chunk = g_total_cap / 2;
    if (chunk < 16ULL * 1024 * 1024) chunk = 16ULL * 1024 * 1024;
    chunk = (chunk + ALIGN - 1) & ~(ALIGN - 1);
    if (chunk < min_chunk) chunk = min_chunk;

    g_growing = true;
    void* mem = nullptr;
    for (;;) {
        mem = PMM::AllocBytes(chunk);
        if (mem || chunk == min_chunk) break;
        chunk /= 2;
        if (chunk < min_chunk) chunk = min_chunk;
    }
    if (mem) {
        heap_add_region(mem, chunk);
        SerialLogger::Log("Heap: grew by ");
        SerialLogger::LogDec((uint32_t)(chunk / (1024 * 1024)));
        SerialLogger::Log(" MB (total ");
        SerialLogger::LogDec((uint32_t)(g_total_cap / (1024 * 1024)));
        SerialLogger::Log(" MB)\r\n");
    }
    g_growing = false;
    return mem != nullptr;
}

HeapBlock* KernelHeap::FindFree(size_t size) {
    // first-fit over the freelist - O(k) where k = #free blocks, vs the
    // prior O(n) over every block. every hop is validated (link_ok) so a
    // trampled link is detected + truncated instead of dereferenced - the
    // unvalidated walk here was the rip=0x3 wild-jump crash vector. (satoru)
    // a big request walks only the big list; a small one the small list, then
    // the big one (any big block fits it). (satoru)
    for (uint64_t which = (size >= HEAP_BIG_BLOCK) ? 1 : 0; which < 2; which++) {
        FreeLink** head = freelist_head(which);
        FreeLink* prev = nullptr;
        for (FreeLink* l = *head; l; l = l->next) {
            if (!link_ok(l)) {
                heap_warn("Heap: freelist corrupt link in find (truncated)\r\n");
                if (prev) prev->next = nullptr; else *head = nullptr;
                break;
            }
            HeapBlock* b = block_from_link(l);
            if (b->size >= size) return b;
            prev = l;
        }
    }
    return nullptr;
}

// kurono.memcensus=1: a side table of live allocations (pointer -> caller
// return address + size), so the census can name who holds the heap. off (null)
// on a normal boot; maintained under the heap's own guard. open addressing with
// tombstones; when it fills, further allocations go uncounted. (satoru)
struct HeapCensusSlot { uint64_t ptr; uint32_t site; uint32_t size; };
static HeapCensusSlot* g_hc = nullptr;
static constexpr uint32_t HC_CAP = 1u << 17;          // 2 mb of slots (satoru)
static constexpr uint64_t HC_TOMB = 1;
static uint32_t g_hc_overflow = 0;
static inline uint32_t hc_hash(uint64_t p) { return (uint32_t)((p >> 4) * 2654435761u) & (HC_CAP - 1); }
static void hc_add(void* ptr, size_t size, void* ra) {
    if (!g_hc || !ptr) return;
    uint64_t p = (uint64_t)(uintptr_t)ptr;
    uint32_t h = hc_hash(p);
    for (uint32_t i = 0; i < 64; i++) {
        HeapCensusSlot& s = g_hc[(h + i) & (HC_CAP - 1)];
        if (s.ptr == 0 || s.ptr == HC_TOMB) {
            s.ptr = p; s.site = (uint32_t)(uintptr_t)ra;
            s.size = size > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)size;
            return;
        }
    }
    g_hc_overflow++;
}
static void hc_del(void* ptr) {
    if (!g_hc || !ptr) return;
    uint64_t p = (uint64_t)(uintptr_t)ptr;
    uint32_t h = hc_hash(p);
    for (uint32_t i = 0; i < 64; i++) {
        HeapCensusSlot& s = g_hc[(h + i) & (HC_CAP - 1)];
        if (s.ptr == p) { s.ptr = HC_TOMB; return; }
        if (s.ptr == 0) return;
    }
}
void KernelHeap::CensusEnable() {
    if (g_hc) return;
    g_hc = (HeapCensusSlot*)PMM::AllocBytes(sizeof(HeapCensusSlot) * HC_CAP);   // zeroed (satoru)
}
uint32_t KernelHeap::CensusCapacity() { return g_hc ? HC_CAP : 0; }
uint32_t KernelHeap::CensusOverflow() { return g_hc_overflow; }
bool KernelHeap::CensusEntry(uint32_t i, uint32_t* site, uint32_t* size) {
    if (!g_hc || i >= HC_CAP) return false;
    uint64_t p = g_hc[i].ptr;
    if (p == 0 || p == HC_TOMB) return false;
    *site = g_hc[i].site; *size = g_hc[i].size;
    return true;
}

// the synchronous heap_grow runs inside whatever allocation hit the end: the
// pmm's contiguous scan + zero of a 16-54 mb chunk took 40-130 ms there, inside
// kls-held syscalls (firefox's sqlite/indexeddb writes growing kvfs content),
// and the gui waited ~100 ms behind them. this refill does the same growth from
// the logging kernel process with NO lock held across the pmm call: it peeks
// under the heap guard, takes the chunk from the pmm unlocked, installs it under
// the guard. kurono.heapreserve=0 turns it off. (satoru)
static bool g_reserve_on = true;
void KernelHeap::RefillReserve() {
    if (!g_reserve_on || !expanded) return;
    size_t largest = 0, total = 0;
    {
        HeapIrqGuard _g;
        if (g_growing || g_reg_count >= HEAP_MAX_REGIONS - 1) return;
        total = g_total_cap;
        int hops = 0;
        for (FreeLink* l = g_free_big; l && hops < 4096; l = l->next, hops++) {
            if (!link_ok(l)) break;
            HeapBlock* b = block_from_link(l);
            if (b->size > largest) largest = b->size;
        }
    }
    const size_t ALIGN = 2ULL * 1024 * 1024;
    size_t want = total / 8;
    if (want < 16ULL * 1024 * 1024) want = 16ULL * 1024 * 1024;
    if (largest >= want) return;
    size_t chunk = total / 2;
    if (chunk < 16ULL * 1024 * 1024) chunk = 16ULL * 1024 * 1024;
    chunk = (chunk + ALIGN - 1) & ~(ALIGN - 1);
    void* mem = PMM::AllocBytes(chunk);   // no heap guard held (satoru)
    if (!mem) return;
    bool added = false;
    {
        HeapIrqGuard _g;
        if (g_reg_count < HEAP_MAX_REGIONS) { heap_add_region(mem, chunk); added = true; }
    }
    if (!added) { PMM::FreeBytes(mem, chunk); return; }
    SerialLogger::Log("Heap: reserve grew by ");
    SerialLogger::LogDec((uint32_t)(chunk / (1024 * 1024)));
    SerialLogger::Log(" MB (total ");
    SerialLogger::LogDec((uint32_t)(g_total_cap / (1024 * 1024)));
    SerialLogger::Log(" MB)\r\n");
}
void KernelHeap::SetReserve(bool on) { g_reserve_on = on; }

void* KernelHeap::Alloc(size_t size) {
    return AllocFrom(size, __builtin_return_address(0));
}

void* KernelHeap::AllocFrom(size_t size, void* site) {
    if (g_heap_direct && expanded && size >= HEAP_DIRECT_MIN && size < (1ULL << 40)) {
        // the pmm call (and its zeroing) runs without the heap lock (satoru)
        size_t payload = (size + HEAP_FOOTER_SIZE + 15) & ~(size_t)15;
        size_t bytes = (HEAP_HEADER_SIZE + payload + 4095) & ~(size_t)4095;
        uint8_t* mem = (uint8_t*)PMM::AllocBytes(bytes);
        if (mem) {
            HeapIrqGuard _g;
            if (g_direct_n < HEAP_DIRECT_MAX) {
                g_direct[g_direct_n].base = mem;
                g_direct[g_direct_n].bytes = bytes;
                g_direct_n++;
                g_direct_bytes += bytes;
                HeapBlock* b = (HeapBlock*)mem;
                b->size = bytes - HEAP_HEADER_SIZE;
                mark_used(b);
                void* out = mem + HEAP_HEADER_SIZE;
                hc_add(out, size, site);
                if (TaskMem::g_on) TaskMem::OnHeapAlloc(out, b->size, (uint32_t)_g.cpu);
                return out;
            }
            PMM::FreeBytes(mem, bytes);   // table full: the region heap takes it (satoru)
        }
    }
    HeapIrqGuard _g;   // interrupt-safe: blocks IRQ-context re-entry (satoru)
    size_t asked = size;
    if (!initialized) Init();
    if (size == 0) size = 1;
    // reserve the trailing 8-byte boundary-tag footer OUTSIDE the caller's
    // payload. the footer occupies the last 8 bytes of the block and is read
    // by prev_block() during backward coalesce; if it overlapped the caller's
    // usable region (as it did before this fix), a full-size write - e.g. a
    // jpeg decode buffer or a linux-init struct - clobbers it, corrupting the
    // coalesce path and eventually tripping "free() bad magic". round
    // (size + footer) up to 16-byte alignment so block->size always leaves a
    // clear 8 bytes for the footer past the caller's data. (satoru)
    if (size > (size_t)-1 - 15 - HEAP_FOOTER_SIZE) return nullptr;
    size = (size + HEAP_FOOTER_SIZE + 15) & ~(size_t)15;
    if (size < HEAP_MIN_PAYLOAD) size = HEAP_MIN_PAYLOAD;

    HeapBlock* block = FindFree(size);
    if (!block && expanded && heap_grow(size)) block = FindFree(size);
    if (!block) {
        heap_warn("Heap: OOM!\r\n");
        return nullptr;
    }

    freelist_remove(block);

    uint64_t remaining = block->size - size;
    // need room for header + min payload + footer for the split-off remainder.
    if (remaining >= HEAP_HEADER_SIZE + HEAP_MIN_PAYLOAD) {
        HeapBlock* next = (HeapBlock*)((uint8_t*)block + HEAP_HEADER_SIZE + size);
        next->size  = remaining - HEAP_HEADER_SIZE;
        mark_free(next);
        freelist_push(next);
        block->size = size;
    }

    mark_used(block);
    void* out = (void*)((uint8_t*)block + HEAP_HEADER_SIZE);
    hc_add(out, asked, site);
    if (TaskMem::g_on) TaskMem::OnHeapAlloc(out, block->size, (uint32_t)_g.cpu);   // owner side table, no header change (satoru)
    return out;
}

// if u break this nothing crashes right away, it just slowly corrupts everything
void KernelHeap::Free(void* ptr) {
    if (!ptr) return;
    HeapIrqGuard _g;   // interrupt-safe: blocks IRQ-context re-entry (satoru)

    uint8_t* data = (uint8_t*)ptr;

    int region = region_of(data);
    bool in_active = region >= 0;
    bool in_boot   = expanded &&
                     (data >= boot_buffer && data < boot_buffer + BOOT_CAPACITY);
    if (!in_active && !in_boot) {
        int di = direct_find(data);   // a direct (pmm-backed) block (satoru)
        if (di < 0) return;
        HeapBlock* db = (HeapBlock*)(data - HEAP_HEADER_SIZE);
        if (!valid_magic(db->flags) || !block_used(db)) { heap_warn("Heap: direct Free() bad block (ignored)\r\n"); return; }
        hc_del(ptr);
        if (TaskMem::g_on) TaskMem::OnHeapFree(ptr, db->size);
        uint8_t* base = g_direct[di].base;
        size_t bytes = g_direct[di].bytes;
        db->flags = 0;
        g_direct[di] = g_direct[g_direct_n - 1];
        g_direct_n--;
        g_direct_bytes -= bytes;
        PMM::FreeBytes(base, bytes);
        return;
    }

    HeapBlock* block = (HeapBlock*)(data - HEAP_HEADER_SIZE);

    if (!valid_magic(block->flags)) {
        // freeing a pointer whose header isn't a live block (stray/double free
        // of an already-coalesced region). ignore it safely; rate-limited log
        // so it can never cascade into a flood. (satoru)
        heap_warn("Heap: Free() bad magic (ignored)\r\n");
        return;
    }
    if (!block_used(block)) {
        heap_warn("Heap: Double free (ignored)\r\n");
        return;
    }
    hc_del(ptr);
    // sanity check: header size matches footer
    uint64_t fsz = *footer_of(block) & ~(1ULL << 63);
    if (fsz != block->size) {
        heap_warn("Heap: Free() corrupt footer (ignored)\r\n");
        return;
    }
    // charge back to the block's original owner. bootstrap blocks freed after
    // expansion were never in the enable-time baseline, so they are skipped. (satoru)
    if (in_active && TaskMem::g_on) TaskMem::OnHeapFree(ptr, block->size);

    mark_free(block);

    // only coalesce within the block's own region; bootstrap fragments after
    // expansion stay isolated.
    if (!in_active) {
        // no freelist insertion for boot-region frees post-expansion
        return;
    }

    uint8_t* region_base = g_reg_base[region];
    uint8_t* heap_end    = region_base + g_reg_cap[region];

    // forward coalesce - O(1) via footer
    while (true) {
        HeapBlock* n = next_block(heap_end, block);
        if (!n || block_used(n)) break;
        freelist_remove(n);
        block->size += HEAP_HEADER_SIZE + n->size;
        n->flags = 0;
        n->size  = 0;
        mark_free(block);
    }

    // backward coalesce - O(1) via previous footer
    while (true) {
        HeapBlock* p2 = prev_block(region_base, block);
        if (!p2 || block_used(p2)) break;
        freelist_remove(p2);
        p2->size += HEAP_HEADER_SIZE + block->size;
        block->flags = 0;
        block->size  = 0;
        block = p2;
        mark_free(block);
    }

    // an extra region that is now one free block goes back to the pmm, so a
    // launched app's arena is reclaimed when it exits instead of staying
    // reserved for the rest of the boot. the first region stays as the floor. (satoru)
    if (g_reg_count > 1 && block == (HeapBlock*)region_base &&
        block->size == g_reg_cap[region] - HEAP_HEADER_SIZE) {
        size_t cap = g_reg_cap[region];
        g_reg_count--;
        if (region != g_reg_count) {
            g_reg_base[region] = g_reg_base[g_reg_count];
            g_reg_cap[region]  = g_reg_cap[g_reg_count];
        }
        g_total_cap -= cap;
        PMM::FreeBytes(region_base, cap);
        return;
    }

    freelist_push(block);
}

void KernelHeap::Coalesce(HeapBlock* block) {
    // coalescing is folded into Free() for O(1) behaviour; retained for ABI.
    (void)block;
}

void* KernelHeap::Realloc(void* ptr, size_t new_size) {
    if (!ptr) return Alloc(new_size);
    if (new_size == 0) { Free(ptr); return nullptr; }
    HeapIrqGuard _g;   // interrupt-safe; nests harmlessly with Alloc/Free (satoru)
    if (new_size > (size_t)-1 - 15) return nullptr;

    HeapBlock* block = (HeapBlock*)((uint8_t*)ptr - HEAP_HEADER_SIZE);
    if (!valid_magic(block->flags) || !block_used(block)) return nullptr;
    uint64_t old_size = block->size;

    // match Alloc's footer reservation so the grow threshold is computed
    // against the same block geometry (last 8 bytes reserved for the
    // boundary-tag footer, never handed to the caller). (satoru)
    size_t want = (new_size + HEAP_FOOTER_SIZE + 15) & ~(size_t)15;
    if (want < HEAP_MIN_PAYLOAD) want = HEAP_MIN_PAYLOAD;
    if (old_size >= want) return ptr;

    void* new_ptr = AllocFrom(new_size, __builtin_return_address(0));
    if (!new_ptr) return nullptr;

    size_t copy_size = old_size < new_size ? (size_t)old_size : new_size;
    uint8_t* src = (uint8_t*)ptr;
    uint8_t* dst = (uint8_t*)new_ptr;
    for (size_t i = 0; i < copy_size; i++) dst[i] = src[i];

    Free(ptr);
    return new_ptr;
}

void KernelHeap::LockedCall(void (*fn)(void*), void* arg) {
    HeapIrqGuard _g;   // same guard as Alloc/Free: serialized with every heap op (satoru)
    fn(arg);
}

void KernelHeap::Reset() {
    initialized = false;
    g_free_head = nullptr;
    g_free_big = nullptr;   // (satoru)
    Init();
}

size_t KernelHeap::GetTotal() {
    return g_total_cap + g_direct_bytes;   // regions + direct blocks (satoru)
}

size_t KernelHeap::GetUsed() {
    HeapIrqGuard _g;   // interrupt-safe: walk must not race an IRQ-context alloc (satoru)
    size_t used = 0;
    for (int r = 0; r < g_reg_count; r++) {
        uint8_t* ptr = g_reg_base[r];
        uint8_t* end = g_reg_base[r] + g_reg_cap[r];
        while (ptr + HEAP_HEADER_SIZE <= end) {
            HeapBlock* block = (HeapBlock*)ptr;
            if (!valid_magic(block->flags)) break;
            if (block_used(block)) used += block->size;
            ptr += HEAP_HEADER_SIZE + block->size;
            if (block->size == 0) break;
        }
    }
    return used + g_direct_bytes;   // direct blocks are all in use (satoru)
}

size_t KernelHeap::GetFree() {
    size_t total = g_total_cap;
    size_t used  = GetUsed();
    return total > used + HEAP_HEADER_SIZE ? total - used - HEAP_HEADER_SIZE : 0;
}

bool KernelHeap::IsValidBlock(void* ptr) {
    if (!ptr) return false;
    HeapIrqGuard _g;
    uint8_t* data = (uint8_t*)ptr;
    int r = region_of(data);
    if (r < 0 && direct_find(data) >= 0) {   // a direct block (satoru)
        HeapBlock* db = (HeapBlock*)(data - HEAP_HEADER_SIZE);
        return valid_magic(db->flags) && block_used(db);
    }
    if (r < 0 || data < g_reg_base[r] + HEAP_HEADER_SIZE) return false;
    HeapBlock* block = (HeapBlock*)(data - HEAP_HEADER_SIZE);
    return valid_magic(block->flags) && block_used(block);
}

void KernelHeap::LogMemReport(const char* tag) {
    const uint64_t MB = 1024 * 1024;
    SerialLogger::Log("[memrep] ");
    SerialLogger::Log(tag);
    SerialLogger::Log(" ram=");
    SerialLogger::LogDec((uint32_t)(PMM::GetTotalMemory() / MB));
    SerialLogger::Log("MB pmm_free=");
    SerialLogger::LogDec((uint32_t)(PMM::GetFreeMemory() / MB));
    SerialLogger::Log("MB heap_used=");
    SerialLogger::LogDec((uint32_t)(GetUsed() / MB));
    SerialLogger::Log("MB heap_total=");
    SerialLogger::LogDec((uint32_t)(GetTotal() / MB));
    SerialLogger::Log("MB\r\n");
}
