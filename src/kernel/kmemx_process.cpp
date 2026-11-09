#include "kmemx.h"
#include "kmemx_pool.h"
#include "pmm.h"
#include "../proc/scheduler.h"
#include "../drivers/serial.h"
#include "../drivers/graphics.h"
#include "../drivers/timer.h"     // wall-clock telemetry cadence (satoru)

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

// ── scheduling-latency (jitter) instrument ──────────────────────────────────
// this worker asks for a 10ms sleep, so the gap between consecutive ticks is a
// direct probe of how long the cooperative scheduler made it wait. AVERAGE
// tick rate is a bad detector: it came out identical (81/sec) with and without
// the blocking virtio present, because the average absorbs a 1.17ms stall into
// the existing ~2.3ms of slop. the TAIL is where a non-yielding busy-poll must
// show up, so track the worst gap and how many gaps blow past a threshold.
// reported in kilocycles (divide by the cpu MHz that boot logs). (satoru)
static inline uint64_t kx_rdtsc() {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
static uint64_t g_gap_max = 0;      // worst inter-tick gap, cycles (satoru)
static uint64_t g_gap_sum = 0;
static uint32_t g_gap_n   = 0;
static uint32_t g_gap_over = 0;     // gaps > ~15ms at 2.9GHz (satoru)

// periodic telemetry. the engine kept every number it measures inside a struct
// that only the interactive shell + settings ever read, so a headless boot
// batch could never answer "is the engine reaching pages at all". this dumps
// the live counters to serial so a boot log alone tells the whole story:
// pool occupancy (is the 512mb reservation earning its keep), the real
// compression ratio, the fault-path latency band, and the pmm frames actually
// in use. one line per interval, no floats (compiler code is -mno-sse). (satoru)
void log_stats(uint32_t tick) {
    const Stats& s = GetStats();
    uint64_t pool_total = KMemXPool::TotalBytes();
    uint64_t used_mb = (PMM::GetUsedFrames() * 4096ULL) / (1024 * 1024);
    uint64_t free_mb = (PMM::GetFreeFrames() * 4096ULL) / (1024 * 1024);
    uint64_t mean_ns = s.pages_out ? (s.ns_decompress_sum / s.pages_out) : 0;

    // RAW tick count, not tick/100. the worker sleeps 10ms per tick, so this is
    // a direct high-resolution measure of how much bsp time the cooperative
    // scheduler actually gave it (100/sec = perfect). the old tick/100 print
    // quantised to 100 ticks, which buried effects smaller than ~3 ticks/sec -
    // exactly the size of the effect being measured. (satoru)
    // jitter first - it is the point of this line now. (satoru)
    SerialLogger::Log("[kxjit] gapmax_kcyc=");
    SerialLogger::LogDec((int)(g_gap_max / 1000));
    SerialLogger::Log(" gapmean_kcyc=");
    SerialLogger::LogDec((int)(g_gap_n ? (g_gap_sum / g_gap_n / 1000) : 0));
    SerialLogger::Log(" over15ms=");
    SerialLogger::LogDec((int)g_gap_over);
    SerialLogger::Log(" samples=");
    SerialLogger::LogDec((int)g_gap_n);
    // starvation-valve telemetry: did the user-task donation fire, and did it
    // hand a slice over? a persisting stall with donate_ok=0 means the >50ms
    // trigger never tripped, which is a different bug from "donated and it did
    // not help". (satoru)
    {
        uint64_t dc = 0, dok = 0, dnone = 0;
        Scheduler::GetDonateStats(&dc, &dok, &dnone);
        SerialLogger::Log(" donate=");
        SerialLogger::LogDec((int)dc);
        SerialLogger::Log("/ok=");
        SerialLogger::LogDec((int)dok);
        SerialLogger::Log("/gated=");
        SerialLogger::LogDec((int)dnone);
    }
    SerialLogger::Log("\r\n");

    SerialLogger::Log("[kmxstat] tick=");
    SerialLogger::LogDec((int)tick);
    SerialLogger::Log(" live=");
    SerialLogger::LogDec((int)s.live_pages);
    SerialLogger::Log(" poolkb=");
    SerialLogger::LogDec((int)(s.pool_used / 1024));
    SerialLogger::Log("/");
    SerialLogger::LogDec((int)(pool_total / 1024));
    SerialLogger::Log(" ratiox100=");
    SerialLogger::LogDec(RatioX100());
    SerialLogger::Log(" in=");
    SerialLogger::LogDec((int)s.pages_in);
    SerialLogger::Log(" out=");
    SerialLogger::LogDec((int)s.pages_out);
    SerialLogger::Log(" flt=");
    SerialLogger::LogDec((int)s.faults_served);
    SerialLogger::Log(" raw=");
    SerialLogger::LogDec((int)s.compress_fail);
    SerialLogger::Log(" dedup=");
    SerialLogger::LogDec((int)s.dedup_saved);
    SerialLogger::Log(" dns_min/mean/max=");
    SerialLogger::LogDec((int)s.ns_decompress_min);
    SerialLogger::Log("/");
    SerialLogger::LogDec((int)mean_ns);
    SerialLogger::Log("/");
    SerialLogger::LogDec((int)s.ns_decompress_max);
    SerialLogger::Log(" over10us=");
    SerialLogger::LogDec((int)s.decomp_over_10us);
    SerialLogger::Log(" press=");
    SerialLogger::Log(PressureName(CurrentPressure()));
    SerialLogger::Log(" pmm_used_mb=");
    SerialLogger::LogDec((int)used_mb);
    SerialLogger::Log(" pmm_free_mb=");
    SerialLogger::LogDec((int)free_mb);
    SerialLogger::Log("\r\n");
}

[[noreturn]] void kmemx_process_entry() {
    SerialLogger::Log("[KMemX] compression engine process online\r\n");
    uint32_t tick = 0;
    for (;;) {
        // 10ms cadence (the spec's scan interval). SleepMs yields the core; the
        // process consumes no cpu between ticks. (satoru)
        Scheduler::SleepMs(10);
        tick++;
        {
            // sample the gap this tick actually took. skip the first sample
            // (no previous timestamp) and any absurd one from a clock reset.
            // (satoru)
            static uint64_t s_last = 0;
            uint64_t now = kx_rdtsc();
            if (s_last) {
                uint64_t gap = now - s_last;
                if (gap < (1ULL << 40)) {
                    if (gap > g_gap_max) g_gap_max = gap;
                    g_gap_sum += gap;
                    g_gap_n++;
                    if (gap > 43000000ULL) g_gap_over++;   // ~15ms @2.9GHz (satoru)
                }
            }
            s_last = now;
        }

        // telemetry BEFORE the early-outs below: a disabled engine or a busy
        // compositor must still report, otherwise the quietest boots (exactly
        // the ones worth explaining) produce no data at all. (satoru)
        // while the adaptive pool holds chunks (or pressure is up) also report
        // every ~5s of wall time: the worker is often starved far below its
        // nominal 100 ticks/s, so tick-based telemetry alone could miss a whole
        // grow/compress/shrink cycle. (satoru)
        {
            static uint32_t s_last_log_ms = 0;
            uint32_t now_ms = Timer::GetRealMs();
            bool busy_pool = KMemXPool::TotalBytes() > 0 || CurrentPressure() >= PRESS_YELLOW;
            if (tick == 50 || (tick % 500) == 0 ||
                (busy_pool && (uint32_t)(now_ms - s_last_log_ms) >= 5000u)) {
                log_stats(tick);
                s_last_log_ms = now_ms;
            }
        }

        if (!IsEnabled()) {
            // disabled -> idle, no scanning. still let the pool policy hand
            // back any chunk that drained after the engine went off. (satoru)
            if (KMemXPool::TotalBytes() > 0 && (tick % 100) == 0) MaintainPool(CurrentPressure());
            continue;
        }

        // (1) refresh pressure from live free ram. (satoru)
        Pressure p = UpdatePressure();

        // adaptive pool: grow under pressure, shrink after it clears, reclaim
        // orphans. (satoru)
        MaintainPool(p);

        // compress only under pressure. at green the pool is meant to drain
        // and go back to the pmm, so no new pages are taken into it. (satoru)
        if (p == PRESS_GREEN) continue;

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
