#pragma once
#include "types.h"
struct Process;

// kurono.memcensus=1 leak census. the pmm tags every frame with the return
// address of the allocator's caller; a report groups the used frames by that
// site, says how many of each no live user address space maps, and prints the
// change since the previous report next to it. plus the heap, page tables,
// memfd objects and linux process records. symbolize the sites with
// addr2line -f -C -e <the exact kurono_base.elf>. off by default. (satoru)
namespace MemCensus {
    void Init();                       // after the heap is up; no-op unless the pmm tags are on
    bool Enabled();
    void Report(const char* why);
    void StartPeriodic(uint32_t secs);  // also reports once when free memory drops under the low floor
    void SetLowMemFloor(uint32_t mb);
    void CheckLow();                    // one "lowmem" report when free memory first drops under the floor (satoru)   // kurono.memcensus.lowmb=N (0 = off), default 24 (satoru)  // spawns a kernel process that reports every secs
    void RegionTableFull(Process* owner);   // profile of a full region table (census builds) (satoru)
}

// end (satoru)
