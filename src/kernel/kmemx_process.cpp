#include "kmemx.h"
#include "../proc/scheduler.h"
#include "../drivers/serial.h"
#include "../drivers/graphics.h"

//  KMemX dedicated compression kernel-process (stage 4). (satoru)
//
//  the engine's worker. it runs forever at a high kernel-process priority (just
//  below the realtime scheduler heartbeat), wakes every 10ms, and does a small,
//  strictly-bounded amount of work so it is invisible:
//
//    1. recompute pressure (green..critical) from live free ram.
//    2. acquire a token-bucket budget: 1..16 pages this tick, by pressure. one
//       4kb compress is ~1us, so even the 16-page red/critical budget is ~16us
//       of a 10ms tick == ~0.16% of one core; the bucket guarantees the cpu cap
//       holds regardless of how many candidates exist.
//    3. yield IMMEDIATELY if the compositor has pending frames (UIDirtyCount>0)
//       so a scan never delays a render - the single most visible thing.
//    4. scan for aged candidates and compress up to the budget.
//    5. occasionally run a low-priority dedup pass.
//    6. sleep to the next 10ms tick.
//
//  it never holds the engine lock across a sleep, never blocks, and does no i/o,
//  so it cannot stall audio/network/the compositor. (satoru)

namespace KMemX {

namespace {
bool g_proc_started = false;

// per-tick page budget by pressure. green is gentle (compress only the oldest),
// red/critical push the 16-page batch. (satoru)
int budget_for_pressure(Pressure p) {
    switch (p) {
        case PRESS_GREEN:    return 2;    // ~1% - oldest pages only (satoru)
        case PRESS_YELLOW:   return 4;    // ~2% (satoru)
        case PRESS_ORANGE:   return 8;    // ~4% (satoru)
        case PRESS_RED:      return 16;   // ~5% (satoru)
        case PRESS_CRITICAL: return 16;   // emergency - full batch every tick (satoru)
    }
    return 2;
}

// is the compositor mid-frame / about to render? a non-destructive peek at the
// dirty counter (we must NOT consume it - that is the gui loop's job). if there
// is pending visual work, kmemx steps aside this tick. (satoru)
inline bool compositor_busy() {
    return Graphics::UIDirtyCount() > 0;
}

[[noreturn]] void kmemx_process_entry() {
    SerialLogger::Log("[KMemX] compression engine process online\r\n");
    uint32_t tick = 0;
    for (;;) {
        // 10ms cadence (the spec's scan interval). SleepMs yields the core; the
        // process consumes no cpu between ticks. (satoru)
        Scheduler::SleepMs(10);
        tick++;

        if (!IsEnabled()) continue;          // disabled -> idle, no scanning (satoru)

        // (1) refresh pressure from live free ram. (satoru)
        Pressure p = UpdatePressure();

        // (3) never compete with a render: if frames are pending, skip this tick.
        // under green/yellow we are extra polite; under red/critical memory
        // matters more than a frame, so we proceed. (satoru)
        if (compositor_busy() && (p == PRESS_GREEN || p == PRESS_YELLOW)) continue;

        // (2)+(4) acquire the token budget and scan+compress up to it. the scan
        // itself is bounded (it ages a multiple of the budget, then stops), so a
        // single tick is a few microseconds of work. (satoru)
        int budget = budget_for_pressure(p);
        ScanAndCompress(budget);

        // (5) low-priority dedup: every ~2s (200 ticks), one bounded pass. runs
        // last so it only uses spare cycles. (satoru)
        if ((tick % 200) == 0) {
            DedupPass(8);
        }
    }
}
}  // namespace

int TokenBudget() { return budget_for_pressure(CurrentPressure()); }

bool ProcessRunning() { return g_proc_started; }

bool StartProcess() {
    if (g_proc_started) return true;
    if (!IsInitialized()) return false;
    // high kernel-process priority but below the realtime scheduler heartbeat so
    // it never preempts the timebase; a modest stack (it does no deep recursion).
    // (satoru)
    Process* p = Scheduler::SpawnKernelProcess("kmemx", kmemx_process_entry,
                                               PRIO_HIGH, 128, 4096);
    if (!p) {
        SerialLogger::Log("[KMemX] WARN: could not spawn compression process\r\n");
        return false;
    }
    g_proc_started = true;
    return true;
}

}  // namespace KMemX

// end (satoru)
