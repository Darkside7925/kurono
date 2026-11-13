#include "frame_pacer.h"
#include "frame_pacer_policy.h"
#include "../drivers/graphics.h"
#include "../drivers/outputs.h"
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
//
//  the policy itself (which frames count, when to step) lives in
//  frame_pacer_policy.h so the host unit test can drive it with synthetic
//  frame sequences. this file only owns the config, the logging and the
//  hardware/output ceiling. (satoru)

namespace FramePacer {

namespace {

FramePacerPolicy::Pacer g_pacer;

uint32_t g_panel_mhz = 0;
const char* g_panel_src = "default";
uint32_t g_cmdline_hz = 0;
uint32_t g_mode = 0;          // 0 auto, else fixed (satoru)
bool     g_vsync = true;

uint64_t g_tsc_hz = 0;

uint32_t panel_hz_rounded() {
    return g_panel_mhz ? (g_panel_mhz + 500) / 1000 : 0;
}

// the highest refresh any lit output actually shows. a rate above it is
// wasted: the pipe drops every extra frame. 0 when no output reports one.
// (satoru)
uint32_t max_output_hz() {
    uint32_t mhz = Outputs::MaxRefreshMilliHz();
    // single-output boots have no registered output (the firmware framebuffer
    // is drawn directly): the lit primary is the panel that rate detection
    // measured. with outputs registered this never overrides their ceiling.
    // (satoru)
    if (!mhz && Outputs::Connected() == 0) mhz = panel_hz_rounded() * 1000u;
    return mhz ? (mhz + 500) / 1000 : 0;
}

// what the config asks for before the output ceiling is applied. (satoru)
uint32_t requested_cap() {
    if (!g_vsync) return 360;                     // old "vsync off" behaviour (satoru)
    uint32_t want = g_cmdline_hz ? g_cmdline_hz : g_mode;
    if (want == 0) {
        // auto: the panel rate, snapped down so a 239.96 or 143.98 hz panel
        // lands on 240 / 144 (rounded first), and a 59.94 one on 60. (satoru)
        uint32_t p = panel_hz_rounded();
        return FramePacerPolicy::Pacer::SnapDown(p ? p : 60);
    }
    if (want < 60) want = 60;
    if (want > 360) want = 360;
    return want;
}

uint32_t compute_cap() {
    uint32_t want = requested_cap();
    uint32_t maxout = max_output_hz();
    if (maxout && want > maxout) want = maxout;
    if (want < 60) want = 60;
    return want;
}

void apply_rate(const char* why) {
    uint32_t hz = g_pacer.CurrentHz();
    Graphics::SetTargetFPS(hz);   // keeps GetTargetFPS() readers (settings, hud) in step (satoru)
    SerialLogger::Log("[hz] rate ");
    SerialLogger::LogDec((int)hz);
    SerialLogger::Log(" cap=");
    SerialLogger::LogDec((int)g_pacer.CapHz());
    SerialLogger::Log(" panel=");
    SerialLogger::LogDec((int)panel_hz_rounded());
    SerialLogger::Log("(");
    SerialLogger::Log(g_panel_src);
    SerialLogger::Log(") out=");
    SerialLogger::LogDec((int)max_output_hz());
    SerialLogger::Log(" mode=");
    if (g_cmdline_hz) { SerialLogger::Log("cmdline:"); SerialLogger::LogDec((int)g_cmdline_hz); }
    else if (g_mode) { SerialLogger::Log("fixed:"); SerialLogger::LogDec((int)g_mode); }
    else SerialLogger::Log("auto");
    SerialLogger::Log(" (");
    SerialLogger::Log(why);
    SerialLogger::Log(")\r\n");
}

void rebuild_ladder(const char* why) {
    g_pacer.Rebuild(compute_cap(), 0);
    apply_rate(why);
}

void log_decision(FramePacerPolicy::Action act) {
    if (act == FramePacerPolicy::ACT_DOWN) {
        SerialLogger::Log("[hz] step down ");
        SerialLogger::LogDec((int)g_pacer.LastFromHz());
        SerialLogger::Log(" -> ");
        SerialLogger::LogDec((int)g_pacer.LastToHz());
        SerialLogger::Log(" miss_pct=");
        SerialLogger::LogDec((int)g_pacer.LastMissPct());
        SerialLogger::Log(" cost_us=");
        SerialLogger::LogDec((int)(g_pacer.LastCostNs() / 1000));
        SerialLogger::Log(" late_max_us=");
        SerialLogger::LogDec((int)(g_pacer.LastLateNs() / 1000));
        SerialLogger::Log("\r\n");
    } else if (act == FramePacerPolicy::ACT_UP) {
        SerialLogger::Log("[hz] step up ");
        SerialLogger::LogDec((int)g_pacer.LastFromHz());
        SerialLogger::Log(" -> ");
        SerialLogger::LogDec((int)g_pacer.LastToHz());
        SerialLogger::Log(" cost_us=");
        SerialLogger::LogDec((int)(g_pacer.LastCostNs() / 1000));
        SerialLogger::Log(" miss_pct=");
        SerialLogger::LogDec((int)g_pacer.LastMissPct());
        SerialLogger::Log("\r\n");
    }
    Graphics::SetTargetFPS(g_pacer.CurrentHz());
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
    uint32_t old_cap = g_pacer.CapHz();
    g_mode = mode;
    g_vsync = vs;
    // only rebuild (and reset the adaptive state) when the cap really changes:
    // ReloadFromConfig runs on every settings/personalize change. (satoru)
    static bool s_first = true;
    if (s_first || compute_cap() != old_cap) { s_first = false; rebuild_ladder("config"); }
    else Graphics::SetTargetFPS(g_pacer.CurrentHz());
}

void SetMode(uint32_t hz) {
    if (hz != 0 && hz < 60) hz = 60;
    if (hz > 360) hz = 360;
    UIConfig::SetInt("display.refresh_mode", (int)hz, true);
    UIConfig::Save();
    g_mode = hz;
    rebuild_ladder("settings");
}

void OutputsChanged() {
    // an output was lit/off or relit at another timing: the ceiling may have
    // moved. only reset the adaptive state when the cap really changed. (satoru)
    if (compute_cap() == g_pacer.CapHz()) {
        Graphics::SetTargetFPS(g_pacer.CurrentHz());
        return;
    }
    rebuild_ladder("outputs");
}

uint32_t Mode() { return g_mode; }
uint32_t CmdlineHz() { return g_cmdline_hz; }
uint32_t CapHz() { return g_pacer.CapHz(); }
uint32_t CurrentHz() { return g_pacer.CurrentHz(); }
uint64_t PeriodNs() { return g_pacer.PeriodNs(); }

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

void OnFrame(uint64_t cost_ns, uint64_t late_ns, bool continuous, bool real, bool work) {
    FramePacerPolicy::Action act =
        g_pacer.OnFrame(Timer::GetRealMs(), cost_ns, late_ns, continuous, real, work);
    if (act != FramePacerPolicy::ACT_NONE) log_decision(act);
}

}  // namespace FramePacer

// end (satoru)
