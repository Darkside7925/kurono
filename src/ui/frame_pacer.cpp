#include "frame_pacer.h"
#include "../drivers/graphics.h"
#include "../drivers/cpu_detect.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include "../system/ui_config.h"

//  frame pacer - see frame_pacer.h. (satoru)
//
//  why the old pacing could not go above 60 safely: the gui loop slept on a
//  whole-millisecond budget (1000/hz truncated: 240 -> 4 ms = 250 fps, 144 ->
//  6 ms = 166 fps, 120 -> 8 ms) re-armed from "now" each frame, so the cadence
//  was neither the panel rate nor even. the period here is exact nanoseconds
//  on a fixed grid (deadline += period), so 240 averages 240 and a late frame
//  does not push every later one back. (satoru)

namespace FramePacer {

namespace {

const uint32_t kSteps[] = { 240, 165, 144, 120, 90, 60 };
const int kStepCount = (int)(sizeof(kSteps) / sizeof(kSteps[0]));
const uint32_t kMinHz = 60;

uint32_t g_panel_mhz = 0;
const char* g_panel_src = "default";
uint32_t g_cmdline_hz = 0;
uint32_t g_mode = 0;          // 0 auto, else fixed (satoru)
bool     g_vsync = true;

// the ladder of rates the adaptive step walks: [cap, steps below cap...]. (satoru)
uint32_t g_ladder[8] = { 60 };
int      g_ladder_n = 1;
int      g_idx = 0;           // index into the ladder; 0 = cap (satoru)
uint64_t g_period_ns = 16666667ULL;

// adaptive window state. (satoru)
uint32_t g_win_frames = 0, g_win_miss = 0;
uint64_t g_win_cost = 0, g_win_late_max = 0;
int      g_good_windows = 0;
uint32_t g_up_allowed_ms = 0;
uint32_t g_backoff_ms = 10000;

uint64_t g_tsc_hz = 0;

uint32_t panel_hz_rounded() {
    return g_panel_mhz ? (g_panel_mhz + 500) / 1000 : 0;
}

// largest step <= hz (hz itself when it is not a step but still >= 60). (satoru)
uint32_t snap_down(uint32_t hz) {
    if (hz < kMinHz) return kMinHz;
    for (int i = 0; i < kStepCount; i++) if (kSteps[i] <= hz) return kSteps[i];
    return kMinHz;
}

uint32_t compute_cap() {
    if (!g_vsync) return 360;                     // old "vsync off" behaviour (satoru)
    uint32_t want = g_cmdline_hz ? g_cmdline_hz : g_mode;
    if (want == 0) {
        // auto: the panel rate, snapped down so a 239.96 or 143.98 hz panel
        // lands on 240 / 144 (rounded first), and a 59.94 one on 60. (satoru)
        uint32_t p = panel_hz_rounded();
        return snap_down(p ? p : 60);
    }
    if (want < kMinHz) want = kMinHz;
    if (want > 360) want = 360;
    return want;
}

void apply_rate(const char* why) {
    uint32_t hz = g_ladder[g_idx];
    g_period_ns = 1000000000ULL / hz;
    Graphics::SetTargetFPS(hz);   // keeps GetTargetFPS() readers (settings, hud) in step (satoru)
    g_win_frames = g_win_miss = 0;
    g_win_cost = g_win_late_max = 0;
    SerialLogger::Log("[hz] rate ");
    SerialLogger::LogDec((int)hz);
    SerialLogger::Log(" cap=");
    SerialLogger::LogDec((int)g_ladder[0]);
    SerialLogger::Log(" panel=");
    SerialLogger::LogDec((int)panel_hz_rounded());
    SerialLogger::Log("(");
    SerialLogger::Log(g_panel_src);
    SerialLogger::Log(") mode=");
    if (g_cmdline_hz) { SerialLogger::Log("cmdline:"); SerialLogger::LogDec((int)g_cmdline_hz); }
    else if (g_mode) { SerialLogger::Log("fixed:"); SerialLogger::LogDec((int)g_mode); }
    else SerialLogger::Log("auto");
    SerialLogger::Log(" (");
    SerialLogger::Log(why);
    SerialLogger::Log(")\r\n");
}

void rebuild_ladder(const char* why) {
    uint32_t cap = compute_cap();
    g_ladder_n = 0;
    g_ladder[g_ladder_n++] = cap;
    for (int i = 0; i < kStepCount && g_ladder_n < 8; i++) {
        if (kSteps[i] < cap) g_ladder[g_ladder_n++] = kSteps[i];
    }
    g_idx = 0;
    g_good_windows = 0;
    g_backoff_ms = 10000;
    g_up_allowed_ms = 0;
    apply_rate(why);
}

}  // namespace

void SetPanelMilliHz(uint32_t mhz, const char* source) {
    g_panel_mhz = mhz;
    g_panel_src = source ? source : "?";
    SerialLogger::Log("[hz] panel refresh ");
    SerialLogger::LogDec((int)(mhz / 1000));
    SerialLogger::Log(".");
    uint32_t fr = mhz % 1000;
    char d[4] = { (char)('0' + fr / 100), (char)('0' + (fr / 10) % 10), (char)('0' + fr % 10), 0 };
    SerialLogger::Log(d);
    SerialLogger::Log(" Hz from ");
    SerialLogger::Log(g_panel_src);
    SerialLogger::Log("\r\n");
    rebuild_ladder("panel detected");
}

uint32_t PanelHz() { return panel_hz_rounded(); }
const char* PanelSource() { return g_panel_src; }

void SetCmdlineHz(uint32_t hz) {
    g_cmdline_hz = hz;
    rebuild_ladder("kurono.hz");
}

void ReloadConfig() {
    int m = UIConfig::Int("display.refresh_mode", 0);
    if (m < 0) m = 0;
    if (m > 360) m = 360;
    uint32_t mode = (uint32_t)m;
    bool vs = UIConfig::Bool("display.vsync", true);
    uint32_t old_cap = g_ladder[0];
    g_mode = mode;
    g_vsync = vs;
    // only rebuild (and reset the adaptive state) when the cap really changes:
    // ReloadFromConfig runs on every settings/personalize change. (satoru)
    static bool s_first = true;
    if (s_first || compute_cap() != old_cap) { s_first = false; rebuild_ladder("config"); }
    else Graphics::SetTargetFPS(g_ladder[g_idx]);
}

void SetMode(uint32_t hz) {
    if (hz != 0 && hz < kMinHz) hz = kMinHz;
    if (hz > 360) hz = 360;
    UIConfig::SetInt("display.refresh_mode", (int)hz, true);
    UIConfig::Save();
    g_mode = hz;
    rebuild_ladder("settings");
}

uint32_t Mode() { return g_mode; }
uint32_t CmdlineHz() { return g_cmdline_hz; }
uint32_t CapHz() { return g_ladder[0]; }
uint32_t CurrentHz() { return g_ladder[g_idx]; }
uint64_t PeriodNs() { return g_period_ns; }

uint64_t NowNs() {
    if (g_tsc_hz == 0) {
        uint64_t f = CPUDetect::GetInfo().frequency.tsc_frequency;
        if (f < 1000000ULL) return Timer::GetRealMs64() * 1000000ULL;
        g_tsc_hz = f;
    }
    uint64_t c = __builtin_ia32_rdtsc();
    // split so cycles * 1e9 cannot overflow 64 bits. (satoru)
    return (c / g_tsc_hz) * 1000000000ULL + ((c % g_tsc_hz) * 1000000000ULL) / g_tsc_hz;
}

void OnFrame(uint64_t cost_ns, uint64_t late_ns, bool continuous) {
    if (!continuous) return;   // the first frame after an idle gap says nothing (satoru)
    uint64_t period = g_period_ns;
    g_win_frames++;
    g_win_cost += cost_ns;
    if (late_ns > g_win_late_max) g_win_late_max = late_ns;
    if (late_ns > period / 2 || cost_ns > (period * 9) / 10) g_win_miss++;

    uint32_t hz = g_ladder[g_idx];
    uint32_t win = hz / 2;      // ~0.5 s of continuous frames (satoru)
    if (win < 30) win = 30;
    if (g_win_frames < win) return;

    uint32_t now = Timer::GetRealMs();
    uint64_t mean_cost = g_win_cost / g_win_frames;
    uint32_t miss_pct = (g_win_miss * 100) / g_win_frames;
    uint64_t late_max = g_win_late_max;
    g_win_frames = g_win_miss = 0;
    g_win_cost = g_win_late_max = 0;

    if (miss_pct >= 25 && g_idx + 1 < g_ladder_n) {
        // the machine is not keeping up: one step down. the next try at the
        // higher rate waits a backoff that doubles each time (10 s .. 60 s), so
        // a marginal rate cannot oscillate. (satoru)
        uint32_t from = hz;
        g_idx++;
        g_good_windows = 0;
        g_up_allowed_ms = now + g_backoff_ms;
        if (g_backoff_ms < 60000) g_backoff_ms *= 2;
        SerialLogger::Log("[hz] step down ");
        SerialLogger::LogDec((int)from);
        SerialLogger::Log(" -> ");
        SerialLogger::LogDec((int)g_ladder[g_idx]);
        SerialLogger::Log(" miss_pct=");
        SerialLogger::LogDec((int)miss_pct);
        SerialLogger::Log(" cost_us=");
        SerialLogger::LogDec((int)(mean_cost / 1000));
        SerialLogger::Log(" late_max_us=");
        SerialLogger::LogDec((int)(late_max / 1000));
        SerialLogger::Log("\r\n");
        g_period_ns = 1000000000ULL / g_ladder[g_idx];
        Graphics::SetTargetFPS(g_ladder[g_idx]);
        return;
    }
    if (g_idx > 0 && miss_pct == 0 && (int32_t)(now - g_up_allowed_ms) >= 0) {
        // headroom check against the HIGHER rate's budget: the frame must
        // cost under half of it. four clean windows (~2 s) in a row. (satoru)
        uint64_t up_period = 1000000000ULL / g_ladder[g_idx - 1];
        if (mean_cost * 2 < up_period) {
            if (++g_good_windows >= 4) {
                uint32_t from = hz;
                g_idx--;
                g_good_windows = 0;
                SerialLogger::Log("[hz] step up ");
                SerialLogger::LogDec((int)from);
                SerialLogger::Log(" -> ");
                SerialLogger::LogDec((int)g_ladder[g_idx]);
                SerialLogger::Log(" cost_us=");
                SerialLogger::LogDec((int)(mean_cost / 1000));
                SerialLogger::Log("\r\n");
                g_period_ns = 1000000000ULL / g_ladder[g_idx];
                Graphics::SetTargetFPS(g_ladder[g_idx]);
            }
            return;
        }
    }
    g_good_windows = 0;
}

}  // namespace FramePacer

// end (satoru)
