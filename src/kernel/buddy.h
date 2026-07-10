#pragma once
#include "types.h"

//  kurono os - buddy physical-memory allocator
//
//  Sits on top of PMM.  Maintains per-order free lists for power-of-two
//  page-count allocations (4 KB * 2^order).  ORDER_MAX = 11 gives a max
//  contiguous block of 2048 pages = 8 MB (covers HugePage 2 MB and most
//  DMA needs).  On allocation, the smallest available order >= request
//  is split down recursively; on free, buddies are coalesced upward.
//
//  Memory itself is still owned by PMM; the buddy allocator borrows
//  `BUDDY_POOL_BYTES` (default 256 MB) of contiguous frames at Init()
//  and manages them privately.  Frees outside that pool fall back to
//  PMM::FreeFrame().  This makes it side-by-side with the existing
//  bitmap allocator instead of replacing it (safer migration path).
//
//  Statistics are exported via DumpInfo() and /proc/buddyinfo.

#define BUDDY_ORDER_MAX     11      // 2^11 pages = 2048 = 8 MB
#define BUDDY_POOL_BYTES    (256ull * 1024ull * 1024ull)
#define BUDDY_PAGE_SIZE     4096ull

struct BuddyBlock {
    BuddyBlock* next;
    BuddyBlock* prev;
    uint8_t     order;
    uint8_t     in_use;
    uint16_t    magic;          // 0xBDD0 sentinel
    uint32_t    pad;
};

class Buddy {
public:
    static bool Init();                                    // borrow pool from PMM
    static bool IsReady() { return ready; }

    // power-of-two page allocations.  Returns identity-mapped pointer.
    static void* AllocPages(int order);                    // 2^order pages
    static void  FreePages(void* ptr, int order);

    // byte-sized convenience: rounds up to nearest order.
    static void* AllocBytes(uint64_t bytes);
    static void  FreeBytes(void* ptr, uint64_t bytes);

    // huge-page helpers (2 MB = order 9, 1 GB = order 18 - out of pool).
    static void* AllocHugePage();                          // 2 MB
    static void  FreeHugePage(void* ptr);

    // statistics
    static uint64_t GetPoolBase()    { return pool_base; }
    static uint64_t GetPoolBytes()   { return pool_bytes; }
    static uint64_t GetUsedBytes()   { return used_bytes; }
    static uint64_t GetFreeBytes()   { return pool_bytes - used_bytes; }
    static uint32_t GetFreeCount(int order);               // # blocks at order
    static uint32_t GetSplitCount()  { return split_count; }
    static uint32_t GetMergeCount()  { return merge_count; }
    static uint32_t GetAllocCount()  { return alloc_count; }
    static uint32_t GetFreeOpsCount(){ return free_count;  }

    // /proc/buddyinfo formatter (Linux-style, one line per "zone")
    static int DumpProcInfo(char* out, int max_len);

    // ascii diagnostic dump (kernel debug)
    static int DumpInfo(char* out, int max_len);

private:
    static int OrderForBytes(uint64_t bytes);
    static BuddyBlock* PopFree(int order);
    static void        PushFree(BuddyBlock* b, int order);
    static void        Unlink(BuddyBlock* b, int order);
    static BuddyBlock* BuddyOf(BuddyBlock* b, int order);
    static bool        InPool(uint64_t addr);

    static bool        ready;
    static uint64_t    pool_base;
    static uint64_t    pool_bytes;
    static uint64_t    used_bytes;
    static BuddyBlock* free_lists[BUDDY_ORDER_MAX + 1];
    static uint32_t    free_counts[BUDDY_ORDER_MAX + 1];
    static uint32_t    split_count;
    static uint32_t    merge_count;
    static uint32_t    alloc_count;
    static uint32_t    free_count;
};
