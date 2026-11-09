#pragma once
#include "types.h"

//  memory accounting for the task manager and the linux /proc view. (satoru)
//
//  everything here is READ-ONLY and derived from state the kernel already
//  keeps: the pmm frame bitmap + refcounts, the user page tables, the kernel
//  heap arena, the kmemx stats and the memfd table. no hot path (mmap, fault,
//  fork, exit) calls into this file and no struct Process field is added; the
//  only state is a small side table of the last page-table walk per address
//  space. walks take no lock at all (the same racy-by-design read-only walk the
//  memcensus uses; every table pointer is bounded by the top of ram, so a table
//  freed mid-walk reads garbage numbers, never faults).
namespace MemAcct {

// resident memory of one user address space, in kb. (satoru)
struct SpaceMem {
    uint32_t rss_kb;      // present user pages whose frame the pmm owns (shared pages counted fully, like linux rss)
    uint32_t shared_kb;   // the part of rss that is a MAP_SHARED / memfd mapping (PTE_SHARED)
    uint32_t pss_kb;      // proportional share: each page divided by its pmm refcount (sums to unique ram across spaces)
    uint32_t pt_kb;       // page-table pages under the user half of the space
};

// last-walk value for the address space `root` if it is younger than
// max_age_ms, else walk it now (a few ms for a 2 gb firefox, never under a
// lock). false if root is not a plausible address space. with allow_walk
// false it only ever returns a cached value (false when there is none), so a
// caller that is over its time budget never pays for a walk. (satoru)
bool QuerySpace(uint64_t root, uint32_t max_age_ms, SpaceMem* out, bool allow_walk = true);

// sum of pss over every address space walked in the last max_age_ms: the
// unique user ram the task manager has seen, with shared pages counted once. (satoru)
uint32_t SumUserPssKb(uint32_t max_age_ms);

// system-wide ram, straight from the pmm. all in kb. (satoru)
struct SysMem {
    uint64_t total_kb;       // usable ram the pmm manages
    uint64_t free_kb;
    uint64_t used_kb;        // total - free
    uint64_t heap_arena_kb;  // kernel heap arena taken from the pmm (used or not)
    uint64_t kmemx_pool_kb;  // kmemx compressed pool
    uint64_t shm_kb;         // memfd / wl_shm objects
    uint64_t installed_kb;   // firmware-map ram (may exceed total under kurono.memcap)
};
void QuerySystem(SysMem* out);

// kernel heap bytes in use, recomputed at most every 5 s (the heap walk
// disables interrupts, so it must not run on every proc read). (satoru)
uint64_t HeapUsedKbCached();

// /proc/meminfo text from the numbers above. returns bytes written. (satoru)
int FormatMeminfo(char* buf, int mx);

}   // namespace MemAcct

// end (satoru)
