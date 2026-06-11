#include "heap.h"
#include "pmm.h"
#include "../drivers/serial.h"

//  kernel heap  -  segregated free-list allocator with coalescing
//
//  phase 1: 64 kb bootstrap buffer (in bss  -  always available)
//  phase 2: pmm-backed region  -  up to 50% of physical ram
//
//  Each block has a HeapBlock header followed by `size` bytes of payload.
//  Free blocks store a doubly-linked-list pointer pair in the payload
//  area so allocation is O(1) freelist-pop, free is O(1) link-in, and
//  coalesce becomes O(1) by checking neighbour blocks via address
//  arithmetic.  A footer mirroring `size` is written just before each
//  block to enable backward coalescing without scanning the whole heap.

// 64 kb bootstrap heap (in bss  -  zeroed by boot asm). 16-byte aligned so
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
};

// header is 16 bytes; minimum free-block payload must fit FreeLink (16) and
// the 8-byte trailing footer side-by-side. Round up to 16-alignment → 32.
#define HEAP_MIN_PAYLOAD  32
#define HEAP_FOOTER_SIZE  8

static FreeLink* g_free_head = nullptr;

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
// logging path then amplifies one corruption to the saturated budget). cli/sti
// (NOT a spinlock) is the correct primitive: the contention is process-vs-IRQ
// on a single cpu, so a spinlock would deadlock. saves/restores IF so nested
// guards don't prematurely re-enable. (satoru)
struct HeapIrqGuard {
    uint64_t flags;
    HeapIrqGuard()  { __asm__ __volatile__("pushfq; pop %0; cli" : "=r"(flags) :: "memory"); }
    ~HeapIrqGuard() { if (flags & 0x200ULL) __asm__ __volatile__("sti" ::: "memory"); }
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

static void freelist_push(HeapBlock* b) {
    FreeLink* l = (FreeLink*)((uint8_t*)b + HEAP_HEADER_SIZE);
    l->prev = nullptr;
    l->next = g_free_head;
    if (g_free_head) g_free_head->prev = l;
    g_free_head = l;
}

static void freelist_remove(HeapBlock* b) {
    FreeLink* l = (FreeLink*)((uint8_t*)b + HEAP_HEADER_SIZE);
    if (l->prev) l->prev->next = l->next;
    else         g_free_head   = l->next;
    if (l->next) l->next->prev = l->prev;
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
    freelist_push(first);

    initialized = true;
    SerialLogger::Log("Heap: Bootstrap (64 KB BSS)\r\n");
}

void KernelHeap::ExpandWithPMM() {
    if (expanded) return;
    if (!initialized) Init();

    uint64_t total_phys = PMM::GetTotalMemory();

    uint64_t target = total_phys / 2;
    // cap the eager heap grab at 1 gb. it used to take 2 gb contiguous up
    // front, which starved the raw pmm frame pool that ring-3 processes need
    // for demand-paged brk/mmap/stacks (large static binaries like ffmpeg hit
    // pmm oom). 1 gb is far more than the desktop+kvfs ever use. (satoru)
    const uint64_t MAX_HEAP = 1ULL * 1024 * 1024 * 1024;
    const uint64_t MIN_HEAP = 32ULL * 1024 * 1024;
    if (target > MAX_HEAP) target = MAX_HEAP;

    void* big = nullptr;
    while (target >= MIN_HEAP) {
        big = PMM::AllocBytes((size_t)target);
        if (big) break;
        target /= 2;
    }

    if (!big) {
        SerialLogger::Log("Heap: PMM expand FAILED  -  staying on 64 KB bootstrap\r\n");
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

    // abandon the bootstrap freelist; allocations in bss remain valid because
    // their headers don't move. New allocations come from the expanded pool.
    g_free_head = nullptr;
    freelist_push(first);

    uint32_t mb = (uint32_t)(target / (1024 * 1024));
    SerialLogger::Log("Heap: Expanded to ");
    SerialLogger::LogDec(mb);
    SerialLogger::Log(" MB (PMM-backed, ");
    SerialLogger::LogDec((uint32_t)(total_phys / (1024*1024)));
    SerialLogger::Log(" MB physical RAM detected)\r\n");
}

HeapBlock* KernelHeap::FindFree(size_t size) {
    // first-fit over the freelist  -  O(k) where k = #free blocks, vs the
    // prior O(n) over every block.
    for (FreeLink* l = g_free_head; l; l = l->next) {
        HeapBlock* b = block_from_link(l);
        if (b->size >= size) return b;
    }
    return nullptr;
}

void* KernelHeap::Alloc(size_t size) {
    HeapIrqGuard _g;   // interrupt-safe: blocks IRQ-context re-entry (satoru)
    if (!initialized) Init();
    if (size == 0) size = 1;
    // reserve the trailing 8-byte boundary-tag footer OUTSIDE the caller's
    // payload. the footer occupies the last 8 bytes of the block and is read
    // by prev_block() during backward coalesce; if it overlapped the caller's
    // usable region (as it did before this fix), a full-size write  -  e.g. a
    // jpeg decode buffer or a linux-init struct  -  clobbers it, corrupting the
    // coalesce path and eventually tripping "free() bad magic". round
    // (size + footer) up to 16-byte alignment so block->size always leaves a
    // clear 8 bytes for the footer past the caller's data. (satoru)
    if (size > (size_t)-1 - 15 - HEAP_FOOTER_SIZE) return nullptr;
    size = (size + HEAP_FOOTER_SIZE + 15) & ~(size_t)15;
    if (size < HEAP_MIN_PAYLOAD) size = HEAP_MIN_PAYLOAD;

    HeapBlock* block = FindFree(size);
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
    return (void*)((uint8_t*)block + HEAP_HEADER_SIZE);
}

void KernelHeap::Free(void* ptr) {
    if (!ptr) return;
    HeapIrqGuard _g;   // interrupt-safe: blocks IRQ-context re-entry (satoru)

    uint8_t* data = (uint8_t*)ptr;

    bool in_active = (data >= heap_base && data < heap_base + heap_capacity);
    bool in_boot   = expanded &&
                     (data >= boot_buffer && data < boot_buffer + BOOT_CAPACITY);
    if (!in_active && !in_boot) return;

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
    // sanity check: header size matches footer
    uint64_t fsz = *footer_of(block) & ~(1ULL << 63);
    if (fsz != block->size) {
        heap_warn("Heap: Free() corrupt footer (ignored)\r\n");
        return;
    }

    mark_free(block);

    // only coalesce within the active heap region; bootstrap fragments after
    // expansion stay isolated.
    uint8_t* p = (uint8_t*)block;
    if (p < heap_base || p >= heap_base + heap_capacity) {
        // no freelist insertion for boot-region frees post-expansion
        return;
    }

    uint8_t* heap_end = heap_base + heap_capacity;

    // forward coalesce  -  O(1) via footer
    while (true) {
        HeapBlock* n = next_block(heap_end, block);
        if (!n || block_used(n)) break;
        freelist_remove(n);
        block->size += HEAP_HEADER_SIZE + n->size;
        n->flags = 0;
        n->size  = 0;
        mark_free(block);
    }

    // backward coalesce  -  O(1) via previous footer
    while (true) {
        HeapBlock* p2 = prev_block(heap_base, block);
        if (!p2 || block_used(p2)) break;
        freelist_remove(p2);
        p2->size += HEAP_HEADER_SIZE + block->size;
        block->flags = 0;
        block->size  = 0;
        block = p2;
        mark_free(block);
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

    void* new_ptr = Alloc(new_size);
    if (!new_ptr) return nullptr;

    size_t copy_size = old_size < new_size ? (size_t)old_size : new_size;
    uint8_t* src = (uint8_t*)ptr;
    uint8_t* dst = (uint8_t*)new_ptr;
    for (size_t i = 0; i < copy_size; i++) dst[i] = src[i];

    Free(ptr);
    return new_ptr;
}

void KernelHeap::Reset() {
    initialized = false;
    g_free_head = nullptr;
    Init();
}

size_t KernelHeap::GetTotal() {
    return heap_capacity;
}

size_t KernelHeap::GetUsed() {
    HeapIrqGuard _g;   // interrupt-safe: walk must not race an IRQ-context alloc (satoru)
    size_t used = 0;
    uint8_t* ptr = heap_base;
    uint8_t* end = heap_base + heap_capacity;

    while (ptr + HEAP_HEADER_SIZE <= end) {
        HeapBlock* block = (HeapBlock*)ptr;
        if (!valid_magic(block->flags)) break;
        if (block_used(block)) used += block->size;
        ptr += HEAP_HEADER_SIZE + block->size;
        if (block->size == 0) break;
    }
    return used;
}

size_t KernelHeap::GetFree() {
    size_t total = heap_capacity;
    size_t used  = GetUsed();
    return total > used + HEAP_HEADER_SIZE ? total - used - HEAP_HEADER_SIZE : 0;
}

bool KernelHeap::IsValidBlock(void* ptr) {
    if (!ptr) return false;
    HeapIrqGuard _g;
    uint8_t* data = (uint8_t*)ptr;
    if (data < heap_base + HEAP_HEADER_SIZE || data >= heap_base + heap_capacity) return false;
    HeapBlock* block = (HeapBlock*)(data - HEAP_HEADER_SIZE);
    return valid_magic(block->flags) && block_used(block);
}
