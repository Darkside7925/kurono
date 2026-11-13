#pragma once
#include <stdint.h>

//  pure frame-pacer policy. no kernel calls, no timer, no graphics: the gui
//  loop (and the host unit test) feeds it frame outcome samples and it decides
//  the paced rate. kept separate from frame_pacer.cpp so the state machine can
//  be tested on the host with synthetic sequences. (satoru)
//
//  one sample is one composited iteration:
//   - real:      a real desktop frame. boot animation / hand-off frames are not
//                real and reset the window (a window must be continuous real
//                frames only).
//   - continuous: the previous iteration also rendered, so there was no idle
//                gap between the two. an idle fallback frame after a gap still
//                counts as idle capacity (that is how an idle desktop recovers);
//                a work frame after a gap does not count.
//   - work:      this frame had something to draw (damage or an active
//                animation). an idle fallback frame has none and never counts
//                as load, so an idle desktop cannot push the rate down.
//
//  the rate is min(cap, highest lit output refresh), stepped down when >=25%
//  of the frames that had work missed their budget, and back up after clean
//  windows. an all-idle window is spare capacity and counts toward recovery,
//  so a load spike cannot pin the desktop at a low rate. (satoru)
namespace FramePacerPolicy {

enum Action { ACT_NONE = 0, ACT_DOWN = 1, ACT_UP = 2 };

class Pacer {
public:
    static const int kMaxLadder = 8;

    // cap_hz = what the user/config asks for, max_output_hz = the highest
    // refresh among the lit outputs (0 = unknown). rebuilds the ladder and
    // resets the adaptive state. (satoru)
    void Rebuild(uint32_t cap_hz, uint32_t max_output_hz) {
        if (max_output_hz != 0 && cap_hz > max_output_hz) cap_hz = max_output_hz;
        if (cap_hz < kMinHz) cap_hz = kMinHz;
        cap = cap_hz;
        n = 0;
        ladder[n++] = cap_hz;
        for (int i = 0; i < 6 && n < kMaxLadder; i++)
            if (kSteps[i] < cap_hz) ladder[n++] = kSteps[i];
        idx = 0;
        period_ns = 1000000000ull / ladder[0];
        good_windows = 0;
        backoff_ms = 10000;
        up_allowed_ms = 0;
        reset_window();
        last_action = ACT_NONE;
        last_from = last_to = ladder[0];
        last_miss_pct = 0;
        last_cost_ns = last_late_ns = 0;
    }

    // true when this sample completed a window that changed the rate; the
    // last_* fields then describe the decision for the [hz] line. (satoru)
    Action OnFrame(uint32_t now_ms, uint64_t cost_ns, uint64_t late_ns,
                   bool continuous, bool real, bool work) {
        if (!real) { reset_window(); return ACT_NONE; }
        if (!continuous) {
            // a gap before a frame that had work says nothing about whether
            // the rate was sustained. an idle fallback frame after a gap is
            // exactly what recovery needs to count, so it starts/continues an
            // idle window instead of resetting it. (satoru)
            if (work || win_frames > 0) {
                reset_window();
                if (work) return ACT_NONE;
            }
        }

        if (win_start_ms == 0) win_start_ms = now_ms;
        if (work) {
            win_frames++;
            win_cost += cost_ns;
            if (late_ns > win_late_max) win_late_max = late_ns;
            if (late_ns > period_ns / 2 || cost_ns > (period_ns * 9) / 10) win_miss++;
        } else {
            win_idle++;
        }

        uint32_t hz = ladder[idx];
        uint32_t win = hz / 2;                 // ~0.5 s of frames (satoru)
        if (win < 30) win = 30;
        // an idle desktop only gets a fallback frame every ~250 ms, so its
        // window is wall-clock based: 0.5 s of no work is one idle window.
        bool idle_done = (win_frames == 0 && win_idle > 0 && (uint32_t)(now_ms - win_start_ms) >= 500u);
        if (!idle_done && win_frames + win_idle < win) return ACT_NONE;

        uint32_t now = now_ms;
        uint64_t mean_cost = win_frames ? win_cost / win_frames : 0;
        uint32_t miss_pct = win_frames ? (win_miss * 100) / win_frames : 0;
        uint32_t misses = win_miss;
        uint64_t late_max = win_late_max;
        uint32_t work_frames = win_frames;
        bool idle_only = (win_frames == 0);
        reset_window();

        // a handful of isolated slow frames (a window opening, one toast) is
        // not sustained load: require at least 4 misses in the window too.
        if (!idle_only && misses >= 4 && miss_pct >= 25 && idx + 1 < n) {
            // the machine is not keeping up: one step down. the next try at the
            // higher rate waits a backoff that doubles each time (10 s .. 60 s),
            // so a marginal rate cannot oscillate. (satoru)
            last_action = ACT_DOWN;
            last_from = hz;
            idx++;
            last_to = ladder[idx];
            last_miss_pct = miss_pct;
            last_cost_ns = mean_cost;
            last_late_ns = late_max;
            period_ns = 1000000000ull / ladder[idx];
            good_windows = 0;
            up_allowed_ms = now + backoff_ms;
            if (backoff_ms < 60000) backoff_ms *= 2;
            return ACT_DOWN;
        }

        if (idx > 0) {
            // recovery against the HIGHER rate's budget. frames that had work
            // must cost under half of it; an all-idle window is free capacity.
            // load dropping recovers in 4 clean windows (~2 s), a pure idle
            // desktop in 8 (~4 s), and idle recovery may bypass the backoff a
            // step-down left behind. (satoru)
            bool allowed = idle_only || (int32_t)(now - up_allowed_ms) >= 0;
            uint64_t up_period = 1000000000ull / ladder[idx - 1];
            bool cost_ok = idle_only || (mean_cost * 2 < up_period);
            if (allowed && miss_pct == 0 && cost_ok) {
                int need = idle_only ? 8 : 4;
                if (++good_windows >= need) {
                    last_action = ACT_UP;
                    last_from = hz;
                    idx--;
                    last_to = ladder[idx];
                    last_miss_pct = miss_pct;
                    last_cost_ns = mean_cost;
                    last_late_ns = late_max;
                    (void)work_frames;
                    period_ns = 1000000000ull / ladder[idx];
                    good_windows = 0;
                    return ACT_UP;
                }
                return ACT_NONE;
            }
        }
        good_windows = 0;
        last_miss_pct = miss_pct;
        last_cost_ns = mean_cost;
        last_late_ns = late_max;
        return ACT_NONE;
    }

    uint32_t CapHz() const { return ladder[0]; }
    uint32_t CurrentHz() const { return ladder[idx]; }
    uint64_t PeriodNs() const { return period_ns; }

    // last decision, for the single [hz] log line. (satoru)
    Action LastAction() const { return last_action; }
    uint32_t LastFromHz() const { return last_from; }
    uint32_t LastToHz() const { return last_to; }
    uint32_t LastMissPct() const { return last_miss_pct; }
    uint64_t LastCostNs() const { return last_cost_ns; }
    uint64_t LastLateNs() const { return last_late_ns; }

    // the ladder step just below `hz`, 0 when none. used by callers that want
    // to show what the adaptive policy can do next. (satoru)
    static uint32_t SnapDown(uint32_t hz) {
        if (hz < kMinHz) return kMinHz;
        for (int i = 0; i < 6; i++) if (kSteps[i] <= hz) return kSteps[i];
        return kMinHz;
    }

private:
    static const uint32_t kMinHz = 60;
    static const uint32_t kSteps[6];

    uint32_t cap = 60;
    uint32_t ladder[kMaxLadder] = { 60 };
    int n = 1;
    int idx = 0;
    uint64_t period_ns = 16666667ull;

    uint32_t win_frames = 0, win_idle = 0, win_miss = 0;
    uint32_t win_start_ms = 0;
    uint64_t win_cost = 0, win_late_max = 0;
    int good_windows = 0;
    uint32_t up_allowed_ms = 0;
    uint32_t backoff_ms = 10000;

    Action last_action = ACT_NONE;
    uint32_t last_from = 60, last_to = 60, last_miss_pct = 0;
    uint64_t last_cost_ns = 0, last_late_ns = 0;

    void reset_window() { win_frames = win_idle = win_miss = 0; win_start_ms = 0; win_cost = win_late_max = 0; }
};

const uint32_t Pacer::kSteps[6] = { 240, 165, 144, 120, 90, 60 };

}  // namespace FramePacerPolicy

// end (satoru)
