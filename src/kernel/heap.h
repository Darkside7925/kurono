#pragma once
#include "types.h"

//  kernel heap - free-list allocator with coalescing
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
    static void* AllocFrom(size_t size, void* site);   // Alloc with an explicit census site (operator new) (satoru)
    // kurono.memcensus=1 live-allocation side table (satoru)
    static void     CensusEnable();
    static uint32_t CensusCapacity();
    static uint32_t CensusOverflow();
    static bool     CensusEntry(uint32_t i, uint32_t* site, uint32_t* size);
    static void* Realloc(void* ptr, size_t new_size);
    static void  Free(void* ptr);
    static void  Reset();
    // run fn(arg) under the heap's own lock (taskmem side tables) (satoru)
    static void  LockedCall(void (*fn)(void*), void* arg);

    // true if ptr is the payload of a live (allocated) heap block. lets a
    // caller that may hold a stale/non-heap pointer (e.g. content deserialized
    // from a saved image, or already freed elsewhere) avoid a stray Free()
    // that would trip "bad magic" and corrupt the allocator. (satoru)
    static bool  IsValidBlock(void* ptr);

    // statistics
    static size_t GetUsed();
    static size_t GetFree();
    static size_t GetTotal();

    // one-line "[memrep] <tag> ..." serial summary of pmm + heap use, for
    // tracking boot-time memory cost at fixed milestones. (satoru)
    static void LogMemReport(const char* tag);
    // background heap reserve (the logging kernel process, every 500 ms, no kls):
    // keep one big free block so a syscall rarely has to grow the heap itself. (satoru)
    static void RefillReserve();
    static void SetReserve(bool on);   // kurono.heapreserve=0 (satoru)

private:
    static void Coalesce(HeapBlock* block);
    static HeapBlock* FindFree(size_t size);

    // bootstrap heap (small, bss-resident, 16-byte aligned)
    alignas(16) static uint8_t  boot_buffer[];
    static const size_t BOOT_CAPACITY;

    // active heap region (starts as boot_buffer, switches to pmm region)
    static uint8_t* heap_base;
    static size_t   heap_capacity;
    static bool     initialized;
    static bool     expanded;
};
