#pragma once
#include "types.h"

// kernel-maintained shared time page for the userspace vdso clock. mirrors the
// linux vvar/vdso_time_data page: the timer tick writes the current monotonic +
// realtime seconds/nanoseconds under a seqlock, and the page is mapped READ-ONLY
// into every process just below the vdso code page, so musl's
// __vdso_clock_gettime / __vdso_gettimeofday read it in userspace with ZERO
// syscall. this is the fix for the ~85x boot slowdown: firefox does ~127k
// syscalls/boot and its clock_gettime deadline-poll convoy was paying a full
// serialized kls syscall per call (linux serves them free from this page). (satoru)
namespace KernelVdso {
    // allocate + zero the shared time page and prime it. safe to call once
    // after the timer clock is up. (satoru)
    void Init();
    // refresh the time page from the monotonic + realtime clocks. called from
    // OnTimerTick (irq context) - must stay cheap + lock-free (a seqlock write,
    // no heap/kvfs/port-io). (satoru)
    void Tick();
    // physical address of the time page (== kernel ptr, low ram is identity
    // mapped) so ld-kurono can map it read-only into a user address space. 0 if
    // not yet initialized. (satoru)
    uint64_t TimePagePhys();
    bool Ready();
}

// end (satoru)
