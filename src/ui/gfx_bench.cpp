//  frame statistics + self-driving cursor / drag benchmark (see gfx_bench.h). (satoru)
#include "gfx_bench.h"
#include "window_manager.h"
#include "boot_anim.h"
#include "frame_pacer.h"
#include "desktop.h"
#include "../drivers/graphics.h"
#include "../drivers/mouse.h"
#include "../drivers/outputs.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"

namespace GfxBench {
namespace {

// work-time histogram: 25 us buckets up to 20 ms, the rest in the last one. (satoru)
constexpr int HB = 800;
constexpr uint32_t HB_US = 25;

struct Acc {
    uint32_t frames;
    uint64_t render_sum, present_sum, work_sum;
    uint32_t render_max, present_max, work_max;
    uint32_t over, late, passes_extra;
    uint32_t full_frames;
    uint64_t area_sum;           // damage scissor: pixels repainted (satoru)
    uint32_t gap_max;
    uint64_t last_end_ns;
    uint32_t hist[HB];
};

void acc_reset(Acc& a) {
    a.frames = 0;
    a.render_sum = a.present_sum = a.work_sum = 0;
    a.render_max = a.present_max = a.work_max = 0;
    a.over = a.late = a.passes_extra = 0;
    a.full_frames = 0;
    a.area_sum = 0;
    a.gap_max = 0;
    a.last_end_ns = 0;
    for (int i = 0; i < HB; i++) a.hist[i] = 0;
}

uint32_t acc_pct(const Acc& a, uint32_t pct) {
    if (!a.frames) return 0;
    uint64_t want = ((uint64_t)a.frames * pct + 99) / 100;
    uint64_t run = 0;
    for (int i = 0; i < HB; i++) {
        run += a.hist[i];
        if (run >= want) return (uint32_t)(i + 1) * HB_US;
    }
    return (uint32_t)HB * HB_US;
}

void acc_add(Acc& a, uint32_t r_us, uint32_t p_us, int passes, uint64_t period_ns, bool cont,
             uint64_t late_ns, uint64_t end_ns) {
    uint32_t w = r_us + p_us;
    a.frames++;
    a.render_sum += r_us; a.present_sum += p_us; a.work_sum += w;
    if (r_us > a.render_max) a.render_max = r_us;
    if (p_us > a.present_max) a.present_max = p_us;
    if (w > a.work_max) a.work_max = w;
    int b = (int)(w / HB_US);
    if (b >= HB) b = HB - 1;
    a.hist[b]++;
    if (period_ns && (uint64_t)w * 1000u > period_ns) a.over++;
    if (cont && period_ns && late_ns * 2 > period_ns) a.late++;
    if (passes > 1) a.passes_extra += (uint32_t)(passes - 1);
    if (Graphics::ScissorEnabled()) {
        uint64_t ar = 0; bool fu = false;
        DesktopEnvironment::LastScissorStats(&ar, &fu);
        a.area_sum += ar;
        if (fu) a.full_frames++;
    }
    if (cont && a.last_end_ns && end_ns > a.last_end_ns) {
        uint64_t g = (end_ns - a.last_end_ns) / 1000u;
        if (g > a.gap_max) a.gap_max = (uint32_t)g;
    }
    a.last_end_ns = end_ns;
}

void log_u(const char* k, uint64_t v) {
    SerialLogger::Log(k);
    SerialLogger::LogDec((int)v);
}

// one result line: [tag] name: frames in ms, fps, work/render/present. (satoru)
void acc_log(const char* tag, const char* name, const Acc& a, uint32_t span_ms) {
    uint32_t n = a.frames ? a.frames : 1;
    SerialLogger::Log(tag);
    if (name) { SerialLogger::Log(name); SerialLogger::Log(":"); }
    log_u(" frames=", a.frames);
    log_u(" ms=", span_ms);
    log_u(" fps=", span_ms ? ((uint64_t)a.frames * 1000u + span_ms / 2) / span_ms : 0);
    log_u(" hz=", FramePacer::CurrentHz());
    log_u(" work_us avg=", a.work_sum / n);
    log_u(" p50=", acc_pct(a, 50));
    log_u(" p99=", acc_pct(a, 99));
    log_u(" max=", a.work_max);
    log_u(" render_us avg=", a.render_sum / n);
    log_u(" max=", a.render_max);
    log_u(" present_us avg=", a.present_sum / n);
    log_u(" max=", a.present_max);
    log_u(" over_budget=", a.over);
    log_u(" late=", a.late);
    log_u(" gap_max_us=", a.gap_max);
    log_u(" extra_passes=", a.passes_extra);
    if (Graphics::ScissorEnabled()) {
        log_u(" repaint_kpx_avg=", a.area_sum / n / 1000u);
        log_u(" full_frames=", a.full_frames);
    }
    SerialLogger::Log("\r\n");
}

bool g_stats = false;
bool g_boot_run = false;
Acc g_sec;                 // the per-second [frame] window (satoru)
uint32_t g_sec_t0 = 0;

// ── benchmark state ───────────────────────────────────────────────────
enum Stage { ST_OFF = 0, ST_WAIT, ST_SETUP, ST_SETTLE, ST_CURSOR, ST_DRAG_AIM, ST_DRAG_PRESS,
             ST_DRAG, ST_DRAG_RELEASE, ST_IDLE, ST_CLEANUP };
Stage g_st = ST_OFF;
uint32_t g_t0 = 0;          // stage start (ms) (satoru)
int g_win_back = -1, g_win_drag = -1;
int g_ox = 0, g_oy = 0, g_ow = 0, g_oh = 0;      // the output the bench runs on (satoru)
int g_out = 0;
int g_dx0 = 0, g_dy0 = 0;   // drag start point (satoru)
Acc g_phase;
bool g_phase_on = false;
uint32_t g_phase_t0 = 0;
uint32_t g_ticks = 0;
char g_summary[200] = {};
uint32_t g_sum_fps[3] = {}, g_sum_p99[3] = {}, g_sum_over[3] = {};

constexpr uint32_t PHASE_MS = 4000;

// sin/cos for the paths: a 5th order polynomial on [-pi, pi] is plenty
// for a pointer path. (satoru)
float fsin(float x) {
    const float PI = 3.14159265f, TWO_PI = 6.2831853f;
    while (x > PI) x -= TWO_PI;
    while (x < -PI) x += TWO_PI;
    float x2 = x * x;
    return x * (1.0f - x2 / 6.0f * (1.0f - x2 / 20.0f * (1.0f - x2 / 42.0f * (1.0f - x2 / 72.0f))));
}
float fcos(float x) { return fsin(x + 1.5707963f); }

void bench_back_render(Window*, int x, int y, int w, int h) {
    Graphics::FillRect(x, y, w, h, 0xFF1E2230u);
    static const char* LINES[4] = {
        "kurono gfx bench: a static window behind the dragged one, with text",
        "the quick brown fox jumps over the lazy dog 0123456789",
        "damage, scissor, flip, cursor plane, blitter, present, vblank",
        "every line here is drawn with the normal text path each frame",
    };
    int row = 0;
    for (int yy = 8; yy + 18 < h; yy += 20, row++)
        Graphics::DrawString(x + 12, y + yy, LINES[row & 3], 0xFFD8DCE8u, 0xFF1E2230u);
}

void bench_drag_render(Window*, int x, int y, int w, int h) {
    for (int b = 0; b < 8; b++) {
        int by = y + (h * b) / 8, bh = (h * (b + 1)) / 8 - (h * b) / 8;
        uint32_t c = 0xFF203048u + (uint32_t)(b * 0x060304);
        Graphics::FillRect(x, by, w, bh, c);
    }
    for (int yy = 40; yy + 48 < h; yy += 56)
        for (int xx = 16; xx + 48 < w; xx += 64)
            Graphics::FillRoundedRect(x + xx, y + yy, 44, 44, 8, ((xx / 64 + yy / 56) & 1) ? 0xFFE8EAF0u : 0xFF4A6090u);
    Graphics::DrawString(x + 16, y + 12, "gfx bench: the dragged window", 0xFFFFFFFFu, 0xFF203048u);
    for (int yy = 40 + 46; yy + 20 < h; yy += 56)
        Graphics::DrawString(x + 16, y + yy, "text between the tiles, drawn every frame", 0xFFC8D0E0u, 0xFF203048u);
}

void pick_output() {
    g_out = 0;
    uint32_t best = 0;
    for (int i = 0; i < Outputs::Total(); i++) {
        if (!Outputs::IsLive(i)) continue;
        uint32_t r = Outputs::RefreshMilliHz(i);
        if (r > best) { best = r; g_out = i; }
    }
    const Output* o = Outputs::Get(g_out);
    if (o && o->connected && o->w > 0 && o->h > 0 && Outputs::Multi()) {
        g_ox = o->x; g_oy = o->y; g_ow = o->w; g_oh = o->h;
    } else {
        g_out = 0;
        g_ox = 0; g_oy = 0; g_ow = Graphics::GetWidth(); g_oh = Graphics::GetHeight();
    }
}

void phase_begin() {
    acc_reset(g_phase);
    g_phase_on = true;
    g_phase_t0 = Timer::GetRealMs();
    g_ticks = 0;
}

void phase_end(const char* name, int idx) {
    g_phase_on = false;
    uint32_t span = Timer::GetRealMs() - g_phase_t0;
    acc_log("[gfxbench] ", name, g_phase, span);
    SerialLogger::Log("[gfxbench] ");
    SerialLogger::Log(name);
    log_u(": pointer updates=", g_ticks);
    log_u(" output=", (uint32_t)g_out);
    log_u(" ", (uint32_t)g_ow);
    log_u("x", (uint32_t)g_oh);
    SerialLogger::Log("\r\n");
    if (idx >= 0 && idx < 3) {
        g_sum_fps[idx] = span ? (uint32_t)(((uint64_t)g_phase.frames * 1000u + span / 2) / span) : 0;
        g_sum_p99[idx] = acc_pct(g_phase, 99);
        g_sum_over[idx] = g_phase.over;
    }
}

void set_pointer(int x, int y) {
    if (x < g_ox) x = g_ox;
    if (y < g_oy) y = g_oy;
    if (x >= g_ox + g_ow) x = g_ox + g_ow - 1;
    if (y >= g_oy + g_oh) y = g_oy + g_oh - 1;
    Mouse::mx = x;
    Mouse::my = y;
    g_ticks++;
}

void sapp(char* d, int cap, const char* s) {
    int n = 0;
    while (n < cap - 1 && d[n]) n++;
    while (n < cap - 1 && *s) d[n++] = *s++;
    d[n] = 0;
}
void sappu(char* d, int cap, uint32_t v) {
    char t[12]; int n = 0;
    if (!v) t[n++] = '0';
    while (v && n < 11) { t[n++] = (char)('0' + v % 10); v /= 10; }
    char r[12]; for (int i = 0; i < n; i++) r[i] = t[n - 1 - i]; r[n] = 0;
    sapp(d, cap, r);
}

}  // namespace

void Configure(bool stats_line, bool run_at_boot) {
    g_stats = stats_line;
    g_boot_run = run_at_boot;
    acc_reset(g_sec);
    if (run_at_boot) Start();
}

bool Running() { return g_st != ST_OFF; }
const char* LastSummary() { return g_summary; }

bool Start() {
    if (g_st != ST_OFF) return false;
    g_st = ST_WAIT;
    g_t0 = Timer::GetRealMs();
    g_summary[0] = 0;
    SerialLogger::Log("[gfxbench] armed: waits for the desktop, then cursor 4 s, drag 4 s, idle 2 s\r\n");
    return true;
}

void Tick() {
    if (g_st == ST_OFF) return;
    uint32_t now = Timer::GetRealMs();
    uint32_t el = now - g_t0;
    switch (g_st) {
    case ST_WAIT:
        if (BootAnim::Active() || BootAnim::CoversScreen()) { g_t0 = now; return; }
        if (el < 3000u) return;
        g_st = ST_SETUP;
        return;
    case ST_SETUP: {
        pick_output();
        int bw = g_ow * 6 / 10, bh = g_oh * 6 / 10;
        int dw = g_ow * 36 / 100, dh = g_oh * 36 / 100;
        if (dw > 960) dw = 960;
        if (dh > 600) dh = 600;
        g_win_back = WindowManager::CreateWindow("gfxbench back", g_ox + g_ow / 16, g_oy + g_oh / 10, bw, bh,
                                                 bench_back_render, nullptr);
        g_win_drag = WindowManager::CreateWindow("gfxbench drag", g_ox + g_ow * 3 / 10, g_oy + g_oh * 3 / 10, dw, dh,
                                                 bench_drag_render, nullptr);
        SerialLogger::Log("[gfxbench] setup: output ");
        SerialLogger::LogDec(g_out);
        log_u(" at ", (uint32_t)g_ox);
        log_u(",", (uint32_t)g_oy);
        log_u(" ", (uint32_t)g_ow);
        log_u("x", (uint32_t)g_oh);
        log_u(" refresh_mhz=", Outputs::RefreshMilliHz(g_out));
        log_u(" pacer_hz=", FramePacer::CurrentHz());
        log_u(" cap_hz=", FramePacer::CapHz());
        SerialLogger::Log("\r\n");
        g_st = ST_SETTLE;
        g_t0 = now;
        return;
    }
    case ST_SETTLE:
        if (el < 1500u || (WindowManager::HasActiveAnimations() && el < 5000u)) return;
        g_st = ST_CURSOR;
        g_t0 = now;
        phase_begin();
        return;
    case ST_CURSOR: {
        // one turn per second on a circle in the middle of the output, over
        // the desktop and both windows. (satoru)
        float a = 6.2831853f * (float)el / 1000.0f;
        int r = g_oh / 3;
        set_pointer(g_ox + g_ow / 2 + (int)((float)r * fcos(a)), g_oy + g_oh / 2 + (int)((float)r * fsin(a)));
        if (el >= PHASE_MS) {
            phase_end("cursor", 0);
            g_st = ST_DRAG_AIM;
            g_t0 = now;
        }
        return;
    }
    case ST_DRAG_AIM: {
        Window* w = WindowManager::GetWindow(g_win_drag);
        if (!w) { SerialLogger::Log("[gfxbench] drag window is gone, skipping the drag\r\n"); g_st = ST_IDLE; g_t0 = now; phase_begin(); return; }
        g_dx0 = w->x + w->w / 2;
        g_dy0 = w->y + 12;
        set_pointer(g_dx0, g_dy0);
        Mouse::BenchButton(true, false, false);
        if (el < 100u) return;
        g_st = ST_DRAG_PRESS;
        g_t0 = now;
        return;
    }
    case ST_DRAG_PRESS:
        set_pointer(g_dx0, g_dy0);
        Mouse::BenchButton(true, true, true);
        if (el < 50u) return;
        g_st = ST_DRAG;
        g_t0 = now;
        phase_begin();
        return;
    case ST_DRAG: {
        // a lissajous path: 300 px side to side every 2 s, 120 px up and down
        // every 1.3 s, so the window keeps moving in both axes. (satoru)
        float t = (float)el;
        int dx = (int)(300.0f * fsin(6.2831853f * t / 2000.0f));
        int dy = (int)(120.0f * fsin(6.2831853f * t / 1300.0f));
        set_pointer(g_dx0 + dx, g_dy0 + dy);
        Mouse::BenchButton(true, true, false);
        if (el >= PHASE_MS) {
            phase_end("drag", 1);
            if (!WindowManager::IsDragging()) SerialLogger::Log("[gfxbench] note: the window manager was not dragging at the end (press missed?)\r\n");
            g_st = ST_DRAG_RELEASE;
            g_t0 = now;
        }
        return;
    }
    case ST_DRAG_RELEASE:
        Mouse::BenchButton(true, false, false);
        if (el < 100u) return;
        Mouse::BenchButton(false, false, false);
        g_st = ST_IDLE;
        g_t0 = now;
        phase_begin();
        return;
    case ST_IDLE:
        if (el < 2000u) return;
        phase_end("idle", 2);
        g_st = ST_CLEANUP;
        g_t0 = now;
        return;
    case ST_CLEANUP:
        if (g_win_drag > 0) WindowManager::CloseWindow(g_win_drag);
        if (g_win_back > 0) WindowManager::CloseWindow(g_win_back);
        g_win_drag = g_win_back = -1;
        g_summary[0] = 0;
        sapp(g_summary, sizeof(g_summary), "gfxbench: cursor fps ");
        sappu(g_summary, sizeof(g_summary), g_sum_fps[0]);
        sapp(g_summary, sizeof(g_summary), " p99 ");
        sappu(g_summary, sizeof(g_summary), g_sum_p99[0]);
        sapp(g_summary, sizeof(g_summary), " us over ");
        sappu(g_summary, sizeof(g_summary), g_sum_over[0]);
        sapp(g_summary, sizeof(g_summary), " | drag fps ");
        sappu(g_summary, sizeof(g_summary), g_sum_fps[1]);
        sapp(g_summary, sizeof(g_summary), " p99 ");
        sappu(g_summary, sizeof(g_summary), g_sum_p99[1]);
        sapp(g_summary, sizeof(g_summary), " us over ");
        sappu(g_summary, sizeof(g_summary), g_sum_over[1]);
        sapp(g_summary, sizeof(g_summary), " | idle fps ");
        sappu(g_summary, sizeof(g_summary), g_sum_fps[2]);
        SerialLogger::Log("[gfxbench] done: ");
        SerialLogger::Log(g_summary);
        SerialLogger::Log("\r\n");
        g_st = ST_OFF;
        Graphics::MarkUIDirty();
        return;
    default:
        g_st = ST_OFF;
        return;
    }
}

void FrameDone(uint64_t render_ns, uint64_t present_ns, int passes,
               uint64_t period_ns, bool continuous, uint64_t late_ns) {
    if (!g_stats && !g_phase_on) return;
    uint32_t r_us = (uint32_t)(render_ns / 1000u), p_us = (uint32_t)(present_ns / 1000u);
    uint64_t end_ns = FramePacer::NowNs();
    if (g_phase_on) acc_add(g_phase, r_us, p_us, passes, period_ns, continuous, late_ns, end_ns);
    if (!g_stats) return;
    uint32_t now = Timer::GetRealMs();
    if (!g_sec.frames) g_sec_t0 = now;
    acc_add(g_sec, r_us, p_us, passes, period_ns, continuous, late_ns, end_ns);
    uint32_t span = now - g_sec_t0;
    if (span >= 1000u) {
        acc_log("[frame]", nullptr, g_sec, span);
        acc_reset(g_sec);
    }
}

}  // namespace GfxBench

// end (satoru)
