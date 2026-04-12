#include "heap.h"
#include "pmm.h"
#include "../drivers/serial.h"

//  kernel heap  -  free-list allocator with coalescing
//
//  phase 1: 64 kb bootstrap buffer (in bss  -  always available)
//  phase 2: pmm-backed region  -  up to 256 mb or 25% of physical ram

// 64 kb bootstrap heap (in bss  -  zeroed by boot asm)
#define BOOT_HEAP_SIZE (64ULL * 1024)
uint8_t        KernelHeap::boot_buffer[BOOT_HEAP_SIZE];
const size_t   KernelHeap::BOOT_CAPACITY = BOOT_HEAP_SIZE;

uint8_t*       KernelHeap::heap_base     = nullptr;
size_t         KernelHeap::heap_capacity = 0;
bool           KernelHeap::initialized   = false;
bool           KernelHeap::expanded      = false;

void KernelHeap::Init() {
    if (initialized) return;

    heap_base     = boot_buffer;
    heap_capacity = BOOT_HEAP_SIZE;

    HeapBlock* first = (HeapBlock*)heap_base;
    first->size  = heap_capacity - HEAP_HEADER_SIZE;
    first->flags = HEAP_MAGIC_FREE;

    initialized = true;
    SerialLogger::Log("Heap: Bootstrap (64 KB BSS)\r\n");
}

//
// strategy: try progressively smaller contiguous allocations until one
// succeeds.  the free-list heap is used for small/medium allocations.
// very large single allocations (multi-mb) should use pmm::allocbytes()
// directly  -  the free-list heap is for general-purpose kernel objects.
//
void KernelHeap::ExpandWithPMM() {
    if (expanded) return;
    if (!initialized) Init();

    uint64_t total_phys = PMM::GetTotalMemory();

    // target: 50% of physical ram (the rest stays in pmm for large allocs)
    // try the target, then halve repeatedly until it works (min 32 mb)
    uint64_t target = total_phys / 2;
    const uint64_t MAX_HEAP = 2ULL * 1024 * 1024 * 1024;  // 2 gb max free-list
    const uint64_t MIN_HEAP = 32ULL * 1024 * 1024;         // 32 mb min
    if (target > MAX_HEAP) target = MAX_HEAP;

    void* big = nullptr;
    while (target >= MIN_HEAP) {
        big = PMM::AllocBytes((size_t)target);
        if (big) break;
        target /= 2;  // halve and retry
    }

    if (!big) {
        SerialLogger::Log("Heap: PMM expand FAILED  -  staying on 64 KB bootstrap\r\n");
        return;
    }

    // don't migrate bootstrap allocations  -  they stay in bss and remain
    // valid.  we just abandon the bootstrap free-space (~64 kb, trivial)
    // and switch all future allocations to the big pmm region.
    uint8_t* new_base = (uint8_t*)big;
    size_t   new_cap  = (size_t)target;

    // one giant free block
    HeapBlock* first = (HeapBlock*)new_base;
    first->size  = new_cap - HEAP_HEADER_SIZE;
    first->flags = HEAP_MAGIC_FREE;

    // switch to the new region
    heap_base     = new_base;
    heap_capacity = new_cap;
    expanded      = true;

    // log result
    uint32_t mb = (uint32_t)(target / (1024 * 1024));
    SerialLogger::Log("Heap: Expanded to ");
    SerialLogger::LogDec(mb);
    SerialLogger::Log(" MB (PMM-backed, ");
    SerialLogger::LogDec((uint32_t)(total_phys / (1024*1024)));
    SerialLogger::Log(" MB physical RAM detected)\r\n");
}

HeapBlock* KernelHeap::FindFree(size_t size) {
    uint8_t* ptr = heap_base;
    uint8_t* end = heap_base + heap_capacity;

    while (ptr + HEAP_HEADER_SIZE <= end) {
        HeapBlock* block = (HeapBlock*)ptr;

        // sanity: if flags are corrupt, bail
        uint64_t magic = block->flags & ~HEAP_BLOCK_USED;
        if (magic != (HEAP_MAGIC_FREE & ~HEAP_BLOCK_USED) &&
            magic != (HEAP_MAGIC_USED & ~HEAP_BLOCK_USED)) {
            break;  // corrupted heap  -  stop scanning
        }

        if (!(block->flags & HEAP_BLOCK_USED) && block->size >= size) {
            return block;
        }

        ptr += HEAP_HEADER_SIZE + block->size;
    }
    return nullptr;
}

void* KernelHeap::Alloc(size_t size) {
    if (!initialized) Init();
    if (size == 0) size = 1;

    // align to 16 bytes
    size = (size + 15) & ~(size_t)15;

    HeapBlock* block = FindFree(size);
    if (!block) {
        SerialLogger::Log("Heap: OOM!\r\n");
        return nullptr;
    }

    // split the block if there's enough leftover for another block + some data
    uint64_t remaining = block->size - size;
    if (remaining > HEAP_HEADER_SIZE + 16) {
        // create a new free block after the allocated region
        HeapBlock* next = (HeapBlock*)((uint8_t*)block + HEAP_HEADER_SIZE + size);
        next->size  = remaining - HEAP_HEADER_SIZE;
        next->flags = HEAP_MAGIC_FREE;

        block->size = size;
    }

    block->flags = HEAP_MAGIC_USED;
    return (void*)((uint8_t*)block + HEAP_HEADER_SIZE);
}

void KernelHeap::Free(void* ptr) {
    if (!ptr) return;

    uint8_t* data = (uint8_t*)ptr;

    // accept pointers from either the active heap or the bootstrap region
    bool in_active = (data >= heap_base && data < heap_base + heap_capacity);
    bool in_boot   = expanded &&
                     (data >= boot_buffer && data < boot_buffer + BOOT_CAPACITY);
    if (!in_active && !in_boot) return;  // not our memory

    HeapBlock* block = (HeapBlock*)(data - HEAP_HEADER_SIZE);

    // validate magic
    if ((block->flags & ~HEAP_BLOCK_USED) != (HEAP_MAGIC_USED & ~HEAP_BLOCK_USED)) {
        SerialLogger::Log("Heap: Free() bad magic!\r\n");
        return;
    }
    if (!(block->flags & HEAP_BLOCK_USED)) {
        SerialLogger::Log("Heap: Double free!\r\n");
        return;
    }

    block->flags = HEAP_MAGIC_FREE;

    // only coalesce if the block is in the active heap region
    // (bootstrap blocks after expansion are just marked free but not coalesced)
    uint8_t* data_check = (uint8_t*)block;
    if (data_check >= heap_base && data_check < heap_base + heap_capacity) {
        Coalesce(block);
    }
}

void KernelHeap::Coalesce(HeapBlock* block) {
    // forward coalesce: merge with the block immediately after
    uint8_t* next_ptr = (uint8_t*)block + HEAP_HEADER_SIZE + block->size;
    if (next_ptr + HEAP_HEADER_SIZE <= heap_base + heap_capacity) {
        HeapBlock* next = (HeapBlock*)next_ptr;
        if ((next->flags & ~HEAP_BLOCK_USED) == (HEAP_MAGIC_FREE & ~HEAP_BLOCK_USED) &&
            !(next->flags & HEAP_BLOCK_USED)) {
            // absorb next block
            block->size += HEAP_HEADER_SIZE + next->size;
            // poison the absorbed header
            next->flags = 0;
            next->size  = 0;
        }
    }

    // backward coalesce: scan from the beginning to find the block before us
    // (we don't have back-pointers, so this is o(n). in practice the heap
    // is small enough that this is fine. a doubly-linked list is future work.)
    uint8_t* scan = heap_base;
    while (scan + HEAP_HEADER_SIZE < (uint8_t*)block) {
        HeapBlock* prev = (HeapBlock*)scan;
        uint64_t magic = prev->flags & ~HEAP_BLOCK_USED;
        if (magic != (HEAP_MAGIC_FREE & ~HEAP_BLOCK_USED) &&
            magic != (HEAP_MAGIC_USED & ~HEAP_BLOCK_USED)) {
            break;  // corrupted  -  stop
        }

        uint8_t* prev_end = scan + HEAP_HEADER_SIZE + prev->size;
        if (prev_end == (uint8_t*)block) {
            // prev is immediately before block
            if (!(prev->flags & HEAP_BLOCK_USED)) {
                prev->size += HEAP_HEADER_SIZE + block->size;
                block->flags = 0;
                block->size  = 0;
            }
            break;
        }
        scan = prev_end;
    }
}

void* KernelHeap::Realloc(void* ptr, size_t new_size) {
    if (!ptr) return Alloc(new_size);
    if (new_size == 0) { Free(ptr); return nullptr; }

    HeapBlock* block = (HeapBlock*)((uint8_t*)ptr - HEAP_HEADER_SIZE);
    uint64_t old_size = block->size;

    // if the current block is already big enough, just return it
    new_size = (new_size + 15) & ~(size_t)15;
    if (old_size >= new_size) return ptr;

    // allocate new block, copy, free old
    void* new_ptr = Alloc(new_size);
    if (!new_ptr) return nullptr;

    // copy min(old_size, new_size) bytes
    size_t copy_size = old_size < new_size ? old_size : new_size;
    uint8_t* src = (uint8_t*)ptr;
    uint8_t* dst = (uint8_t*)new_ptr;
    for (size_t i = 0; i < copy_size; i++) dst[i] = src[i];

    Free(ptr);
    return new_ptr;
}

void KernelHeap::Reset() {
    initialized = false;
    Init();
}

size_t KernelHeap::GetTotal() {
    return heap_capacity;
}

size_t KernelHeap::GetUsed() {
    size_t used = 0;
    uint8_t* ptr = heap_base;
    uint8_t* end = heap_base + heap_capacity;

    while (ptr + HEAP_HEADER_SIZE <= end) {
        HeapBlock* block = (HeapBlock*)ptr;
        uint64_t magic = block->flags & ~HEAP_BLOCK_USED;
        if (magic != (HEAP_MAGIC_FREE & ~HEAP_BLOCK_USED) &&
            magic != (HEAP_MAGIC_USED & ~HEAP_BLOCK_USED)) break;

        if (block->flags & HEAP_BLOCK_USED) {
            used += block->size;
        }
        ptr += HEAP_HEADER_SIZE + block->size;
    }
    return used;
}

size_t KernelHeap::GetFree() {
    return heap_capacity - GetUsed() - HEAP_HEADER_SIZE;  // approximate
}
