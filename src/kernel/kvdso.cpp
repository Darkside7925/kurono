#include "kvdso.h"
#include "pmm.h"
#include "../drivers/timer.h"
#include "time.h"

// see kvdso.h. the field offsets in VdsoTime MUST match the hand-assembled
// userspace reader stubs in ld_kurono.cpp build_vdso (seq@0, mono_sec@8,
// mono_nsec@16, real_sec@24, real_nsec@32). do not reorder. (satoru)
namespace KernelVdso {
namespace {
    struct VdsoTime {
        volatile uint32_t seq;        // +0  seqlock: odd = mid-update (satoru)
        uint32_t          pad;        // +4
        volatile uint64_t mono_sec;   // +8
        volatile uint64_t mono_nsec;  // +16
        volatile uint64_t real_sec;   // +24
        volatile uint64_t real_nsec;  // +32
    };
    VdsoTime* g_page = nullptr;
    uint64_t  g_phys = 0;
}

uint64_t TimePagePhys() { return g_phys; }
bool     Ready()        { return g_page != nullptr; }

void Init() {
    if (g_page) return;
    void* pg = PMM::AllocBytes(PAGE_SIZE);
    if (!pg) return;
    memset(pg, 0, PAGE_SIZE);
    g_phys = (uint64_t)(uintptr_t)pg;   // low ram is identity-mapped: ptr == phys (satoru)
    g_page = (VdsoTime*)pg;
    Tick();
}

void Tick() {
    if (!g_page) return;
    // monotonic ms from the tsc-based clock (no port io -> irq-safe). realtime =
    // the whole-second base (rtc epoch + ntp) plus the monotonic uptime; sub-ms
    // precision is not needed for firefox's deadline polls (coarse, like linux
    // CLOCK_*_COARSE). (satoru)
    uint64_t ms      = Timer::GetRealMs64();
    uint64_t mono_s  = ms / 1000ull;
    uint64_t mono_ns = (ms % 1000ull) * 1000000ull;
    uint64_t real_s  = TimeManager::RealtimeBaseSeconds() + mono_s;

    // seqlock write: bump odd, publish data, bump even. volatile fields + x86
    // TSO keep store order; the release fences are compiler barriers here. a
    // userspace reader that samples an odd seq (or a changed seq across the
    // read) retries. (satoru)
    uint32_t s = g_page->seq;
    g_page->seq = s + 1;                              // odd
    __atomic_thread_fence(__ATOMIC_RELEASE);
    g_page->mono_sec  = mono_s;
    g_page->mono_nsec = mono_ns;
    g_page->real_sec  = real_s;
    g_page->real_nsec = mono_ns;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    g_page->seq = s + 2;                              // even
}

}  // namespace KernelVdso

// end (satoru)
