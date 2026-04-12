#pragma once
#include "types.h"

//  kernel heap  -  free-list allocator with coalescing
//
//  phase 1 (bootstrap): uses a small 64 kb static bss buffer.
//  phase 2 (expanded):  after pmm is ready, expandwithpmm() allocates a
//                       large region from physical memory (up to 256 mb
//                       or 1/4 of available ram, whichever is smaller).
//                       all subsequent allocations use the big pool.

// block header placed before every allocation (16 bytes, naturally aligned)
struct HeapBlock {
    uint64_t size;        // size of the data area (not including this header)
    uint64_t flags;       // bit 0: 1 = used, 0 = free. upper bits: magic.
};

#define HEAP_MAGIC_USED  0xCAFEBEEF00000001ULL
#define HEAP_MAGIC_FREE  0xDEADFEED00000000ULL
#define HEAP_BLOCK_USED  1ULL
#define HEAP_HEADER_SIZE sizeof(HeapBlock)  // 16 bytes

class KernelHeap {
public:
    static void  Init();              // phase 1: bootstrap from bss
    static void  ExpandWithPMM();     // phase 2: allocate big region from pmm
    static void* Alloc(size_t size);
    static void* Realloc(void* ptr, size_t new_size);
    static void  Free(void* ptr);
    static void  Reset();

    // statistics
    static size_t GetUsed();
    static size_t GetFree();
    static size_t GetTotal();

private:
    static void Coalesce(HeapBlock* block);
    static HeapBlock* FindFree(size_t size);

    // bootstrap heap (small, bss-resident)
    static uint8_t  boot_buffer[];
    static const size_t BOOT_CAPACITY;

    // active heap region (starts as boot_buffer, switches to pmm region)
    static uint8_t* heap_base;
    static size_t   heap_capacity;
    static bool     initialized;
    static bool     expanded;
};
