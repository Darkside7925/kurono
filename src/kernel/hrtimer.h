#pragma once
//  kurono os  -  high-resolution timer subsystem (Phase 14)
//
//  small fixed-capacity table of one-shot timers indexed by absolute
//  millisecond deadline. designed for kernel-internal use (deferred
//  work, timeouts, periodic /proc refresh) without dragging in a
//  full red-black tree.
//
//  semantics
//  ---------
//   * deadlines are absolute milliseconds since `Timer::GetRealMs() == 0`.
//   * `Add(name, ms_from_now, cb, ctx)` returns a stable `id` that the
//     caller can pass to `Cancel(id)`. fires at most once.
//   * `AddPeriodic(...)` re-arms automatically after each fire using
//     the original interval; cancel to stop.
//   * `Tick()` is called by the kernel scheduler tick. it fires every
//     timer whose deadline <= now and re-arms periodics. callbacks run
//     in scheduler-tick context  -  keep them short.
//   * `DumpProcInfo(buf, max)` produces a human-readable snapshot
//     compatible with linux `/proc/timer_list`.
//
//  capacity is intentionally small (HRTIMER_MAX = 64). this is plenty
//  for kernel use; userland is expected to use file-based polling.

#include "types.h"

class HRTimer {
public:
    typedef void (*Callback)(uint32_t id, void* ctx);

    static const int HRTIMER_MAX = 64;

    static void Init();

    // returns timer id (>= 1) or 0 on failure (table full).
    static uint32_t Add        (const char* name, uint32_t ms_from_now, Callback cb, void* ctx);
    static uint32_t AddPeriodic(const char* name, uint32_t interval_ms,  Callback cb, void* ctx);

    static bool     Cancel(uint32_t id);

    // Called from scheduler tick (every ~1 ms via Timer::PollUpdate).
    // Idempotent; safe to call from any context that is not itself a
    // timer callback.
    static void     Tick();

    // Snapshot the current timer table into `buf` in /proc/timer_list
    // format. Returns bytes written (excluding NUL).
    static int      DumpProcInfo(char* buf, int max_len);

    // Stats (also exposed via /proc/timer_list footer).
    static uint64_t TotalFires();
    static uint32_t ActiveCount();

    struct Entry {
        bool       active;
        bool       periodic;
        uint32_t   id;
        uint32_t   deadline_ms;
        uint32_t   interval_ms;     // 0 for one-shot
        Callback   cb;
        void*      ctx;
        uint64_t   fires;
        char       name[24];
    };
    static Entry    table[HRTIMER_MAX];

private:
    static uint32_t next_id;
    static uint64_t total_fires;
    static bool     initialized;
};
