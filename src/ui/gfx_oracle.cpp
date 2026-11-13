//  the damage oracle, see gfx_oracle.h. (satoru)
#include "gfx_oracle.h"
#include "window_manager.h"
#include "boot_anim.h"
#include "desktop.h"
#include "control_center.h"
#include "notification.h"
#include "kss.h"
#include "../drivers/graphics.h"
#include "../drivers/intel_accel.h"
#include "../drivers/mouse.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include "../kernel/pmm.h"
#include "../kernel/types.h"

namespace GfxOracle {
namespace {

bool g_on = false;
bool g_shots = false;        // kurono.gfx.oracle.shots=1: hold the partial and the reference frame on screen for screendumps (satoru)
bool g_verbose = true;      // maps and samples of every mismatch (satoru)
bool g_done = false;
int g_sw = 0, g_sh = 0;
int g_win_vid = -1, g_win_app = -1;
bool g_frozen = false;       // the changing window stops changing while a checkpoint settles (satoru)
uint32_t g_vid_n = 0;
bool g_checking = false;     // waiting for the scene to be still, then compare (satoru)
uint32_t g_check_t0 = 0, g_check_frames = 0, g_check_settle = 700;
const char* g_check_name = "";
uint32_t g_frames_total = 0;
uint32_t g_frame_end_ms = 0;   // GetRealMs at the end of the last presented frame (satoru)

// pointer click state machine (satoru)
int g_click_ph = 0; uint32_t g_click_t = 0; int g_click_x = 0, g_click_y = 0;
bool g_click_right = false;
void click_start(int x, int y, bool right) { g_click_ph = 1; g_click_x = x; g_click_y = y; g_click_right = right; g_click_t = Timer::GetRealMs(); }
// true once the click is finished (satoru)
bool click_tick() {
    uint32_t now = Timer::GetRealMs();
    uint32_t el = now - g_click_t;
    Mouse::mx = g_click_x; Mouse::my = g_click_y;
    switch (g_click_ph) {
    case 1:  // aim, button up (satoru)
        Mouse::BenchButton(true, false, false);
        if (el >= 120) { g_click_ph = 2; g_click_t = now; }
        return false;
    case 2:  // press (satoru)
        if (g_click_right) Mouse::BenchRightClick();
        else Mouse::BenchButton(true, true, true);
        g_click_ph = 3; g_click_t = now;
        return false;
    case 3:
        if (el < 100) { if (!g_click_right) Mouse::BenchButton(true, true, false); return false; }
        Mouse::BenchButton(true, false, false);
        g_click_ph = 4; g_click_t = now;
        return false;
    case 4:
        if (el < 100) return false;
        Mouse::BenchButton(false, false, false);
        g_click_ph = 0;
        return true;
    }
    return true;
}

// counters per phase (satoru)
uint64_t g_sc_area = 0, g_sc_full = 0, g_sc_frames = 0;   // scissor passes seen at AfterPresent (satoru)
struct Snap {
    uint64_t sc_area, sc_full, sc_frames;
    Graphics::PresentStats g;
    IntelAccel::FlipTotals f;
    uint32_t frames;
    uint32_t ms;
};
Snap g_snap;
void snap_take(Snap& s) {
    Graphics::GetPresentStats(&s.g);
    IntelAccel::GetFlipTotals(&s.f);
    s.frames = g_frames_total;
    s.sc_area = g_sc_area; s.sc_full = g_sc_full; s.sc_frames = g_sc_frames;
    s.ms = Timer::GetRealMs();
}
void logu(const char* k, uint64_t v) { SerialLogger::Log(k); SerialLogger::LogDec((int)v); }
void phase_report(const char* name) {
    Snap e; snap_take(e);
    uint32_t ms = e.ms - g_snap.ms; if (!ms) ms = 1;
    uint64_t cpu_b = e.g.bytes - g_snap.g.bytes, fl_b = e.f.bytes - g_snap.f.bytes;
    uint64_t tot = cpu_b + fl_b;
    SerialLogger::Log("[oracle] phase "); SerialLogger::Log(name);
    logu(" ms=", ms);
    logu(" frames=", e.frames - g_snap.frames);
    logu(" cpu_present_frames=", e.g.frames - g_snap.g.frames);
    logu(" rects=", e.g.rects - g_snap.g.rects);
    logu(" full_cpu=", e.g.full_frames - g_snap.g.full_frames);
    logu(" flips=", e.f.flips - g_snap.f.flips);
    logu(" full_flips=", e.f.full_flips - g_snap.f.full_flips);
    logu(" flip_defers=", e.f.defers - g_snap.f.defers);
    logu(" flip_merges=", e.f.merges - g_snap.f.merges);
    logu(" dirty_overflows=", e.g.overflows - g_snap.g.overflows);
    logu(" dirty_collapses=", e.g.collapses - g_snap.g.collapses);
    logu(" scissor_full_frames=", e.sc_full - g_snap.sc_full);
    logu(" scissor_kpx_per_frame=", (e.sc_frames - g_snap.sc_frames) ? (e.sc_area - g_snap.sc_area) / (e.sc_frames - g_snap.sc_frames) / 1000u : 0);
    logu(" bytes=", tot);
    logu(" kb_per_s=", tot * 1000u / 1024u / ms);
    SerialLogger::Log("\r\n");
    g_snap = e;
}

// ── checkpoint ───────────────────────────────────────────────────────
uint8_t* g_back = nullptr; uint8_t* g_front = nullptr; size_t g_cap = 0;
uint32_t g_cursor_bad = 0;
uint32_t g_cp_n = 0, g_cp_bad_front = 0, g_cp_bad_back = 0, g_cp_vis_front = 0, g_cp_vis_back = 0;
uint64_t g_worst_front = 0, g_worst_back = 0, g_worst_vis_front = 0, g_worst_vis_back = 0;

struct Diff { uint64_t n; int x0, y0, x1, y1; uint64_t vis; int maxd; };   // vis: pixels off by 16 or more in a channel (satoru)
void diff_init(Diff& d, int sw, int sh) { d.vis = 0; d.maxd = 0; d.n = 0; d.x0 = sw; d.y0 = sh; d.x1 = -1; d.y1 = -1; }

void compare(const uint8_t* a, const uint8_t* b, uint32_t pitch, int sw, int sh,
             int cx, int cy, Diff& d) {
    diff_init(d, sw, sh);
    for (int y = 0; y < sh; y++) {
        const uint32_t* pa = (const uint32_t*)(a + (size_t)y * pitch);
        const uint32_t* pb = (const uint32_t*)(b + (size_t)y * pitch);
        bool cy_in = y >= cy - 2 && y < cy + 30;
        for (int x = 0; x < sw; x++) {
            if (pa[x] == pb[x]) continue;
            if (cy_in && x >= cx - 2 && x < cx + 22) continue;   // the resting software arrow (satoru)
            d.n++;
            {
                int md = 0;
                for (int sft = 0; sft < 24; sft += 8) {
                    int ca = (int)((pa[x] >> sft) & 0xFF), cb = (int)((pb[x] >> sft) & 0xFF);
                    int dd = ca > cb ? ca - cb : cb - ca;
                    if (dd > md) md = dd;
                }
                if (md > d.maxd) d.maxd = md;
                if (md >= 16) d.vis++;
            }
            if (x < d.x0) d.x0 = x;
            if (x > d.x1) d.x1 = x;
            if (y < d.y0) d.y0 = y;
            if (y > d.y1) d.y1 = y;
        }
    }
}

// a coarse map of where the buffers differ: 32 px tiles, '.' none, 1-9 and
// A-Z by how many of the tile's 1024 pixels differ. (satoru)
void log_map(const uint8_t* a, const uint8_t* b, uint32_t pitch, int sw, int sh, int cx, int cy) {
    const int T = 32;
    for (int ty = 0; ty < (sh + T - 1) / T; ty++) {
        char line[96]; int n = 0;
        for (int tx = 0; tx < (sw + T - 1) / T && n < 90; tx++) {
            int cnt = 0;
            for (int y = ty * T; y < ty * T + T && y < sh; y++) {
                const uint32_t* pa = (const uint32_t*)(a + (size_t)y * pitch);
                const uint32_t* pb = (const uint32_t*)(b + (size_t)y * pitch);
                bool cy_in = y >= cy - 2 && y < cy + 30;
                for (int x = tx * T; x < tx * T + T && x < sw; x++) {
                    if (pa[x] == pb[x]) continue;
                    if (cy_in && x >= cx - 2 && x < cx + 22) continue;
                    cnt++;
                }
            }
            line[n++] = !cnt ? '.' : cnt < 10 ? '1' : cnt < 100 ? '2' : cnt < 500 ? '3' : '4';
        }
        line[n] = 0;
        SerialLogger::Log("[oracle]   map "); SerialLogger::Log(line); SerialLogger::Log("\r\n");
    }
}
void log_samples(const uint8_t* a, const uint8_t* b, const uint8_t* c, uint32_t pitch, int sw, int sh, int cx, int cy) {
    int shown = 0;
    for (int y = 0; y < sh && shown < 6; y += 7) {
        const uint32_t* pa = (const uint32_t*)(a + (size_t)y * pitch);
        const uint32_t* pb = (const uint32_t*)(b + (size_t)y * pitch);
        const uint32_t* pc = (const uint32_t*)(c + (size_t)y * pitch);
        for (int x = 0; x < sw && shown < 6; x++) {
            if (pa[x] == pb[x]) continue;
            if (y >= cy - 2 && y < cy + 30 && x >= cx - 2 && x < cx + 22) continue;
            SerialLogger::Log("[oracle]   px ("); SerialLogger::LogDec(x); SerialLogger::Log(","); SerialLogger::LogDec(y);
            SerialLogger::Log(") partial 0x"); SerialLogger::LogHex(pa[x]);
            SerialLogger::Log(" reference 0x"); SerialLogger::LogHex(pb[x]);
            SerialLogger::Log(" front 0x"); SerialLogger::LogHex(pc[x]);
            SerialLogger::Log("\r\n");
            shown++;
            x += 40;
        }
    }
}

// a narrow difference: the pixels of the middle row around it, both buffers (satoru)
void log_row(const uint8_t* a, const uint8_t* b, uint32_t pitch, int x0, int x1, int y) {
    const uint32_t* pa = (const uint32_t*)(a + (size_t)y * pitch);
    const uint32_t* pb = (const uint32_t*)(b + (size_t)y * pitch);
    SerialLogger::Log("[oracle]   row y="); SerialLogger::LogDec(y);
    for (int x = x0 - 4; x <= x1 + 4; x++) {
        if (x < 0) continue;
        SerialLogger::Log(" "); SerialLogger::LogDec(x); SerialLogger::Log(":"); SerialLogger::LogHex(pa[x] & 0xFFFFFF);
        SerialLogger::Log("/"); SerialLogger::LogHex(pb[x] & 0xFFFFFF);
    }
    SerialLogger::Log("\r\n");
}

void log_diff(const char* k, const Diff& d) {
    SerialLogger::Log(k);
    SerialLogger::LogDec((int)d.n);
    if (d.n) {
        SerialLogger::Log(" in ("); SerialLogger::LogDec(d.x0); SerialLogger::Log(",");
        SerialLogger::LogDec(d.y0); SerialLogger::Log(")-("); SerialLogger::LogDec(d.x1 + 1);
        SerialLogger::Log(","); SerialLogger::LogDec(d.y1 + 1); SerialLogger::Log(")");
        SerialLogger::Log(" vis="); SerialLogger::LogDec((int)d.vis);
        SerialLogger::Log(" maxd="); SerialLogger::LogDec(d.maxd);
    }
}

bool scene_moving() {
    return WindowManager::HasActiveAnimations() || Taskbar::IsAnimating() || Desktop::IsAnimating() ||
           ControlCenter::IsAnimating() || KSS::Anim::Active();
}

void do_compare(const char* name, bool unsettled) {
    uint8_t* bb = Graphics::GetBackBuffer();
    uint8_t* fb = Graphics::GetFrontBuffer();
    const int sw = Graphics::GetWidth(), sh = Graphics::GetHeight();
    const uint32_t pitch = Graphics::GetPitch();
    const size_t need = (size_t)pitch * (size_t)sh;
    const bool scaled = Graphics::GetUIScale() != 100;
    if (!bb || !fb || Graphics::GetBpp() != 32 || need > g_cap) {
        SerialLogger::Log("[oracle] cp "); SerialLogger::Log(name); SerialLogger::Log(": skipped (no 32 bpp buffers)\r\n");
        return;
    }
    int ax = -1000, ay = -1000;
    if (!Mouse::LastSoftwarePos(ax, ay)) { ax = -1000; ay = -1000; }
    memcpy(g_back, bb, need);
    if (!scaled) memcpy(g_front, fb, need);
    Timer::HoldUiMs(true, g_frame_end_ms);            // animations step by what the checked frame stepped, nothing more (satoru)
    DesktopEnvironment::RenderReference();           // the truth, in the back buffer (satoru)
    Timer::HoldUiMs(false, 0);
    Diff df, db;
    compare(g_back, bb, pitch, sw, sh, ax, ay, db);
    if (!scaled) compare(g_front, bb, pitch, sw, sh, ax, ay, df); else diff_init(df, sw, sh);
    if (g_verbose && (db.n || (!scaled && df.n))) {
        log_map(g_back, bb, pitch, sw, sh, ax, ay);
        log_samples(g_back, bb, scaled ? g_back : g_front, pitch, sw, sh, ax, ay);
        if (db.n && db.x1 - db.x0 < 4) log_row(g_back, bb, pitch, db.x0, db.x1, (db.y0 + db.y1) / 2);
    }
    if (g_shots && (db.n || (!scaled && df.n))) {
        // diagnosis only: A = what the partial path left on the screen, B = the reference (satoru)
        SerialLogger::Log("[oracle] shot A "); SerialLogger::LogDec((int)g_cp_n + 1); SerialLogger::Log("\r\n");
        uint32_t t = Timer::GetRealMs(); while (Timer::GetRealMs() - t < 2500) { }
        Graphics::MarkFullScreenDirty(); Graphics::SwapBuffers();
        SerialLogger::Log("[oracle] shot B "); SerialLogger::LogDec((int)g_cp_n + 1); SerialLogger::Log("\r\n");
        t = Timer::GetRealMs(); while (Timer::GetRealMs() - t < 2500) { }
    }
    memcpy(bb, g_back, need);                        // leave the state as the partial frames made it (satoru)
    Graphics::ClearDirtyRegions();
    g_cp_n++;
    if (db.n) g_cp_bad_back++;
    if (!scaled && df.n) g_cp_bad_front++;
    if (db.vis) g_cp_vis_back++;
    if (!scaled && df.vis) g_cp_vis_front++;
    if (db.vis > g_worst_vis_back) g_worst_vis_back = db.vis;
    if (df.vis > g_worst_vis_front) g_worst_vis_front = df.vis;
    if (db.n > g_worst_back) g_worst_back = db.n;
    if (df.n > g_worst_front) g_worst_front = df.n;
    SerialLogger::Log("[oracle] cp ");
    SerialLogger::LogDec((int)g_cp_n); SerialLogger::Log(" "); SerialLogger::Log(name);
    log_diff(": back_diff=", db);
    if (scaled) SerialLogger::Log(" front_diff=n/a(scaled)"); else log_diff(" front_diff=", df);
    {   // the arrow must be where the pointer is (logical coordinates) at every ui scale (satoru)
        int px = Mouse::mx, py = Mouse::my, cx2 = -1, cy2 = -1;
        bool have = Mouse::LastSoftwarePos(cx2, cy2);
        bool ok = have && (cx2 - px < 3 && px - cx2 < 3) && (cy2 - py < 3 && py - cy2 < 3);
        SerialLogger::Log(" cursor scale="); SerialLogger::LogDec(Graphics::GetUIScale());
        SerialLogger::Log(" ptr=("); SerialLogger::LogDec(px); SerialLogger::Log(","); SerialLogger::LogDec(py);
        SerialLogger::Log(") arrow=("); SerialLogger::LogDec(cx2); SerialLogger::Log(","); SerialLogger::LogDec(cy2);
        SerialLogger::Log(ok ? ") ok" : ") WRONG");
        if (!ok) g_cursor_bad++;
    }
    SerialLogger::Log(unsettled ? " UNSETTLED" : "");
    SerialLogger::Log("\r\n");
}

void begin_check(const char* name, uint32_t settle_ms) {
    g_checking = true;
    g_frozen = true;
    g_check_t0 = Timer::GetRealMs();
    g_check_frames = 0;
    g_check_name = name;
    g_check_settle = settle_ms;
    SerialLogger::Log("[oracle] check "); SerialLogger::Log(name); SerialLogger::Log("\r\n");
}

// the video-like window: pixels depend on g_vid_n only (satoru)
void vid_render(Window*, int x, int y, int w, int h) {
    for (int b = 0; b < 12; b++) {
        int by = y + (h * b) / 12, bh = (h * (b + 1)) / 12 - (h * b) / 12;
        uint32_t c = 0xFF000000u | ((((uint32_t)b * 21u + g_vid_n * 7u) & 0xFF) << 16) |
                     ((((uint32_t)b * 9u + g_vid_n * 3u) & 0xFF) << 8) | ((g_vid_n * 11u) & 0xFF);
        Graphics::FillRect(x, by, w, bh, c);
    }
    char t[32] = "video frame ";
    int n = 12; uint32_t v = g_vid_n; char r[12]; int k = 0;
    if (!v) r[k++] = '0';
    while (v && k < 11) { r[k++] = (char)('0' + v % 10); v /= 10; }
    while (k) t[n++] = r[--k];
    t[n] = 0;
    Graphics::DrawString(x + 12, y + 12, t, 0xFFFFFFFFu, 0xFF000000u);
}
void app_render(Window*, int x, int y, int w, int h) {
    Graphics::FillRect(x, y, w, h, 0xFF2A2F3Cu);
    for (int yy = 10; yy + 18 < h; yy += 22)
        Graphics::DrawString(x + 12, y + yy, "an app window dragged over the video", 0xFFE0E4F0u, 0xFF2A2F3Cu);
}

float fsin(float x) {
    const float PI = 3.14159265f, TWO_PI = 6.2831853f;
    while (x > PI) x -= TWO_PI;
    while (x < -PI) x += TWO_PI;
    float x2 = x * x;
    return x * (1.0f - x2 / 6.0f * (1.0f - x2 / 20.0f * (1.0f - x2 / 42.0f * (1.0f - x2 / 72.0f))));
}
float fcos(float x) { return fsin(x + 1.5707963f); }

void park() { Mouse::mx = g_sw - 30; Mouse::my = g_sh / 2; }

// ── the script: a list of ops run one after another ──────────────────
enum Kind { K_WAIT_DESKTOP, K_PHASE_BEGIN, K_PHASE_END, K_SLEEP, K_CIRCLE, K_CLICK, K_RCLICK, K_CHECK,
            K_MAKEWIN, K_CC, K_TOAST, K_DRAG_AIM, K_DRAG_MOVE, K_DRAG_REL, K_SCALE, K_END,
            K_HOVER, K_ALTTAB, K_MIN, K_DOWN, K_TO, K_UP };
struct Op {
    Kind k;
    const char* name;          // checkpoint or phase name (satoru)
    uint32_t ms;               // duration or settle time (satoru)
    int arg;                   // which point / open or close / scale (satoru)
};
int g_drag_x0 = 0, g_drag_y0 = 0;
bool g_held = false; int g_hold_x = 0, g_hold_y = 0;
enum Pt { P_LOGO, P_FREE, P_FAR, P_APP_TITLE, P_DOCK, P_CLOSE_BTN, P_APPNAME, P_VOL, P_ICON, P_RB0, P_RB1,
          P_ICON_DST, P_RESIZE, P_RESIZE_DST, P_PARK };
void pt(int which, int* x, int* y) {
    switch (which) {
    case P_LOGO:
        if (Taskbar::TopInset() > 0) { *x = 20; *y = Taskbar::TopInset() / 2; }
        else { *x = 24; *y = Taskbar::GetY() + 12; }
        return;
    case P_FREE: *x = g_sw * 8 / 10; *y = g_sh * 35 / 100; return;
    case P_FAR:  *x = g_sw * 9 / 10; *y = g_sh * 15 / 100; return;
    case P_DOCK: *x = g_sw * 336 / 1000; *y = g_sh - 40; return;
    case P_CLOSE_BTN: {
        Window* w = WindowManager::GetWindow(g_win_app);
        if (w) { *x = w->x + 17; *y = w->y + 12; } else { *x = 80; *y = 300; }
        return;
    }
    case P_APPNAME: *x = 100; *y = Taskbar::TopInset() / 2; return;
    case P_VOL: *x = g_sw - 194; *y = Taskbar::TopInset() / 2; return;
    case P_ICON: *x = 52; *y = 78; return;
    case P_ICON_DST: *x = g_sw * 6 / 10; *y = g_sh * 6 / 10; return;
    case P_RB0: *x = g_sw * 62 / 100; *y = g_sh / 2; return;
    case P_RB1: *x = g_sw * 95 / 100; *y = g_sh * 75 / 100; return;
    case P_RESIZE: {
        Window* w = WindowManager::GetWindow(g_win_app);
        if (w) { *x = w->x + w->w - 2; *y = w->y + w->h - 2; } else { *x = g_sw / 2; *y = g_sh / 2; }
        return;
    }
    case P_RESIZE_DST: *x = g_drag_x0 + 110; *y = g_drag_y0 + 70; return;
    case P_PARK: *x = g_sw - 30; *y = g_sh / 2; return;
    case P_APP_TITLE: {
        Window* w = WindowManager::GetWindow(g_win_app);
        if (w) { *x = w->x + w->w / 2; *y = w->y + 12; } else { *x = g_sw / 2; *y = g_sh / 2; }
        return;
    }
    }
    *x = g_sw / 2; *y = g_sh / 2;
}

const Op SCRIPT[] = {
    { K_WAIT_DESKTOP, "", 0, 0 },
    { K_PHASE_BEGIN, "idle", 0, 0 },     { K_SLEEP, "", 4000, 0 },  { K_PHASE_END, "idle", 0, 0 },
    { K_PHASE_BEGIN, "cursor", 0, 0 },   { K_CIRCLE, "", 4000, 0 }, { K_PHASE_END, "cursor", 0, 0 },
    { K_MAKEWIN, "", 0, 0 },
    { K_CHECK, "base", 1500, 0 },
    { K_CLICK, "", 0, P_LOGO },          { K_CHECK, "menu-open", 700, 0 },
    { K_CLICK, "", 0, P_LOGO },          { K_CHECK, "menu-closed", 700, 0 },
    { K_RCLICK, "", 0, P_FREE },         { K_CHECK, "ctx-open", 700, 0 },
    { K_CLICK, "", 0, P_FAR },           { K_CHECK, "ctx-closed", 700, 0 },
    { K_RCLICK, "", 0, P_APP_TITLE },    { K_CHECK, "wctx-open", 700, 0 },
    { K_CLICK, "", 0, P_FAR },           { K_CHECK, "wctx-closed", 700, 0 },
    { K_CC, "", 0, 1 },                  { K_CHECK, "cc-open", 1200, 0 },
    { K_CC, "", 0, 0 },                  { K_CHECK, "cc-closed", 1200, 0 },
    { K_TOAST, "", 0, 1 },               { K_CHECK, "toast-shown", 1500, 0 },
    { K_TOAST, "", 0, 0 },               { K_CHECK, "toast-gone", 1500, 0 },
    { K_DRAG_AIM, "", 0, 0 },
    { K_PHASE_BEGIN, "drag", 0, 0 },     { K_DRAG_MOVE, "", 3000, 0 }, { K_PHASE_END, "drag", 0, 0 },
    { K_SLEEP, "", 900, 0 },             { K_CHECK, "drag-hold", 300, 0 },
    { K_DRAG_REL, "", 0, 0 },            { K_CHECK, "drag-end", 700, 0 },
    { K_HOVER, "", 900, P_DOCK },        { K_CHECK, "dock-hover", 1500, 0 },
    { K_HOVER, "", 600, P_PARK },        { K_CHECK, "dock-unhover", 1500, 0 },
    { K_HOVER, "", 700, P_CLOSE_BTN },   { K_CHECK, "deco-hover", 1000, 0 },
    { K_HOVER, "", 400, P_PARK },        { K_CHECK, "deco-unhover", 1000, 0 },
    { K_CLICK, "", 0, P_APPNAME },       { K_CHECK, "appmenu-open", 1000, 0 },
    { K_CLICK, "", 0, P_FAR },           { K_CHECK, "appmenu-closed", 1000, 0 },
    { K_CLICK, "", 0, P_VOL },           { K_CHECK, "vol-open", 1000, 0 },
    { K_CLICK, "", 0, P_FAR },           { K_CHECK, "vol-closed", 1000, 0 },
    { K_TOAST, "", 0, 3 },               { K_CHECK, "toast-stack", 1800, 0 },
    { K_TOAST, "", 0, 0 },               { K_CHECK, "toast-stack-gone", 1500, 0 },
    { K_ALTTAB, "", 0, 1 },              { K_CHECK, "tab-open", 1500, 0 },
    { K_ALTTAB, "", 0, 0 },              { K_CHECK, "tab-closed", 1500, 0 },
    { K_MIN, "", 0, 1 },                 { K_CHECK, "minimized", 1800, 0 },
    { K_MIN, "", 0, 0 },                 { K_CHECK, "restored", 1800, 0 },
    { K_DOWN, "", 0, P_ICON },           { K_TO, "", 600, P_ICON_DST },
    { K_SLEEP, "", 500, 0 },             { K_CHECK, "icon-drag-hold", 800, 0 },
    { K_UP, "", 0, 0 },                  { K_CHECK, "icon-dropped", 1000, 0 },
    { K_DOWN, "", 0, P_RB0 },            { K_TO, "", 600, P_RB1 },
    { K_SLEEP, "", 500, 0 },             { K_CHECK, "rubber-hold", 800, 0 },
    { K_UP, "", 0, 0 },                  { K_CHECK, "rubber-end", 1000, 0 },
    { K_DOWN, "", 0, P_RESIZE },         { K_TO, "", 600, P_RESIZE_DST },
    { K_SLEEP, "", 500, 0 },             { K_CHECK, "resize-hold", 800, 0 },
    { K_UP, "", 0, 0 },                  { K_CHECK, "resize-end", 1000, 0 },
    { K_SCALE, "", 0, 125 },             { K_CHECK, "scale125", 1000, 0 },
    { K_CIRCLE, "", 1200, 0 },           { K_CHECK, "scale125-moved", 1000, 0 },
    { K_SCALE, "", 0, 150 },             { K_CHECK, "scale150", 1000, 0 },
    { K_CIRCLE, "", 1200, 0 },           { K_CHECK, "scale150-moved", 1000, 0 },
    { K_SCALE, "", 0, 100 },             { K_CHECK, "scale100", 1500, 0 },
    { K_END, "", 0, 0 },
};
constexpr int NOPS = (int)(sizeof(SCRIPT) / sizeof(SCRIPT[0]));
int g_pc = 0;
bool g_op_started = false;
uint32_t g_op_t0 = 0;

void next() { g_pc++; g_op_started = false; }

void summary() {
    SerialLogger::Log("[oracle] summary: checkpoints="); SerialLogger::LogDec((int)g_cp_n);
    logu(" front_mismatching=", g_cp_bad_front);
    logu(" back_mismatching=", g_cp_bad_back);
    logu(" worst_front_px=", g_worst_front);
    logu(" worst_back_px=", g_worst_back);
    logu(" visible_front_mismatching=", g_cp_vis_front);
    logu(" visible_back_mismatching=", g_cp_vis_back);
    logu(" worst_visible_front_px=", g_worst_vis_front);
    logu(" worst_visible_back_px=", g_worst_vis_back);
    logu(" cursor_wrong=", g_cursor_bad);
    SerialLogger::Log("\r\n[oracle] done\r\n");
}

}  // namespace

uint32_t g_soak_ms = 0, g_soak_t0 = 0, g_loops = 0;
uint32_t g_loop_cps = 0, g_loop_bad = 0;
uint64_t g_pmm_start = 0;
void Configure(bool on, bool shots, int soak_secs) {
    g_on = on;
    g_shots = shots;
    g_soak_ms = (uint32_t)soak_secs * 1000u;
    if (!on) return;
    SerialLogger::Log("[oracle] armed (kurono.gfx.oracle=1): waits for the desktop, then plays the scenario\r\n");
}

bool Running() { return g_on && !g_done; }

void Tick() {
    if (!g_on || g_done) return;
    uint32_t now = Timer::GetRealMs();
    if (!g_frozen && g_win_vid > 0) {
        Window* v = WindowManager::GetWindow(g_win_vid);
        if (v) {
            g_vid_n++;
            Graphics::MarkDirty(v->content_x, v->content_y, v->content_w, v->content_h);
            Graphics::MarkUIDirty();
        }
    }
    if (g_checking) {                       // the compare runs in AfterPresent (satoru)
        // an idle scissored desktop presents nothing at all: ask for a frame
        // so the scene is flushed and the compare gets its turn. (satoru)
        if (now - g_check_t0 >= g_check_settle) Graphics::MarkUIDirty();   // until the compare ran (satoru)
        return;
    }
    if (g_pc >= NOPS) return;
    if (g_held) { Mouse::BenchButton(true, true, false); }
    if (g_pc > 0) { g_sw = Graphics::GetWidth(); g_sh = Graphics::GetHeight(); }
    const Op& op = SCRIPT[g_pc];
    if (!g_op_started) { g_op_started = true; g_op_t0 = now; }
    uint32_t el = now - g_op_t0;
    switch (op.k) {
    case K_WAIT_DESKTOP:
        if (BootAnim::Active() || BootAnim::CoversScreen()) { g_op_t0 = now; return; }
        if (el < 4000) return;
        g_sw = Graphics::GetWidth(); g_sh = Graphics::GetHeight();
        {
            size_t cap = (size_t)Graphics::GetPitch() * (size_t)g_sh;
            g_back = (uint8_t*)PMM::AllocBytes(cap);
            g_front = (uint8_t*)PMM::AllocBytes(cap);
            g_cap = (g_back && g_front) ? cap : 0;
        }
        SerialLogger::Log("[oracle] start "); logu("w=", g_sw); logu(" h=", g_sh);
        SerialLogger::Log(Graphics::ScissorEnabled() ? " scissor=1" : " scissor=0");
        SerialLogger::Log(IntelAccel::FlipActive() ? " flip=1" : " flip=0");
        logu(" top_inset=", Taskbar::TopInset());
        SerialLogger::Log("\r\n");
        park();
        g_soak_t0 = Timer::GetRealMs();
        g_pmm_start = PMM::GetFreeMemory();
        logu("[oracle] pmm_free_kb_start=", g_pmm_start / 1024u);
        SerialLogger::Log("\r\n");
        next();
        return;
    case K_PHASE_BEGIN: snap_take(g_snap); next(); return;
    case K_PHASE_END: phase_report(op.name); next(); return;
    case K_SLEEP:
        if (el >= op.ms) next();
        return;
    case K_CIRCLE: {
        float a = 6.2831853f * (float)el / 1000.0f;
        int r = g_sh / 3;
        Mouse::mx = g_sw / 2 + (int)((float)r * fcos(a));
        Mouse::my = g_sh / 2 + (int)((float)r * fsin(a));
        if (el >= op.ms) { park(); next(); }
        return;
    }
    case K_CLICK: case K_RCLICK: {
        if (g_click_ph == 0) {
            int x, y; pt(op.arg, &x, &y);
            click_start(x, y, op.k == K_RCLICK);
        }
        if (click_tick()) next();
        return;
    }
    case K_CHECK:
        begin_check(op.name, op.ms);
        next();
        return;
    case K_MAKEWIN:
        park();
        g_win_vid = WindowManager::CreateWindow("oracle video", g_sw / 16, g_sh / 6, g_sw * 5 / 12, g_sh * 5 / 12, vid_render, nullptr);
        g_win_app = WindowManager::CreateWindow("oracle app", g_sw / 4, g_sh / 3, g_sw / 3, g_sh / 3, app_render, nullptr);
        next();
        return;
    case K_CC:
        if (op.arg) ControlCenter::Open(); else ControlCenter::Close();
        Graphics::MarkUIDirty();
        park();
        next();
        return;
    case K_TOAST:
        if (op.arg) {
            for (int i = 0; i < op.arg; i++) NotificationManager::Post("Oracle", "a toast for the damage oracle", NotificationManager::ICON_INFO, 60000);
        } else NotificationManager::Clear();
        Graphics::MarkUIDirty();
        next();
        return;
    case K_DRAG_AIM:
        // press the titlebar of the app window through the normal input path (satoru)
        if (g_click_ph == 0 && el < 50) { int x, y; pt(P_APP_TITLE, &x, &y); g_drag_x0 = x; g_drag_y0 = y; }
        Mouse::mx = g_drag_x0; Mouse::my = g_drag_y0;
        if (el < 150) { Mouse::BenchButton(true, false, false); return; }
        if (el < 250) { Mouse::BenchButton(true, true, true); return; }
        Mouse::BenchButton(true, true, false);
        next();
        return;
    case K_DRAG_MOVE: {
        float t = (float)el;
        float f = t / (float)op.ms; if (f > 1.0f) f = 1.0f;
        int dx = (int)(-(float)g_sw * 0.2f * f + (float)g_sw * 0.1f * fsin(6.2831853f * f * 2.0f));
        int dy = (int)((float)g_sh * 0.08f * fsin(6.2831853f * f));
        Mouse::mx = g_drag_x0 + dx; Mouse::my = g_drag_y0 + dy;
        Mouse::BenchButton(true, true, false);
        if (el >= op.ms) next();
        return;
    }
    case K_DRAG_REL:
        Mouse::BenchButton(true, false, false);
        if (el < 150) return;
        Mouse::BenchButton(false, false, false);
        park();
        next();
        return;
    case K_SCALE:
        if (Graphics::GetUIScale() != op.arg) {
            Graphics::RequestUIScale(op.arg);
            if (el < 6000) return;
            SerialLogger::Log("[oracle] note: the ui scale was not applied\r\n");
        }
        park();
        next();
        return;
    case K_HOVER: {
        int x, y; pt(op.arg, &x, &y);
        Mouse::mx = x; Mouse::my = y;
        if (el >= op.ms) next();
        return;
    }
    case K_ALTTAB:
        if (op.arg) WindowManager::AltTabStep(false); else WindowManager::AltTabCancel();
        Graphics::MarkUIDirty();
        next();
        return;
    case K_MIN:
        if (op.arg) WindowManager::Minimize(g_win_app); else WindowManager::Restore(g_win_app);
        Graphics::MarkUIDirty();
        next();
        return;
    case K_DOWN:
        if (el < 50) pt(op.arg, &g_drag_x0, &g_drag_y0);
        Mouse::mx = g_drag_x0; Mouse::my = g_drag_y0;
        if (el < 150) { Mouse::BenchButton(true, false, false); return; }
        if (el < 250) { Mouse::BenchButton(true, true, true); return; }
        g_held = true; g_hold_x = g_drag_x0; g_hold_y = g_drag_y0;
        next();
        return;
    case K_TO: {
        int tx, ty; pt(op.arg, &tx, &ty);
        float f = (float)el / (float)op.ms; if (f > 1.0f) f = 1.0f;
        g_hold_x = g_drag_x0 + (int)((float)(tx - g_drag_x0) * f);
        g_hold_y = g_drag_y0 + (int)((float)(ty - g_drag_y0) * f);
        Mouse::mx = g_hold_x; Mouse::my = g_hold_y;
        if (el >= op.ms) next();
        return;
    }
    case K_UP:
        g_held = false;
        Mouse::BenchButton(true, false, false);
        if (el < 150) return;
        Mouse::BenchButton(false, false, false);
        park();
        next();
        return;
    case K_END:
        if (g_win_app > 0) WindowManager::CloseWindow(g_win_app);
        if (g_win_vid > 0) WindowManager::CloseWindow(g_win_vid);
        g_win_app = g_win_vid = -1;
        Graphics::MarkFullScreenDirty();
        begin_check("windows-closed", 1500);
        next();
        g_pc = NOPS;     // after this check the script is over (satoru)
        return;
    }
}

void AfterPresent() {
    if (!g_on || g_done) return;
    g_frames_total++;
    g_frame_end_ms = Timer::GetRealMs();
    if (Graphics::ScissorEnabled()) {
        uint64_t ar = 0; bool fu = false;
        DesktopEnvironment::LastScissorStats(&ar, &fu);
        g_sc_area += ar; g_sc_frames++; if (fu) g_sc_full++;
    }
    if (!g_checking) return;
    g_check_frames++;
    uint32_t el = Timer::GetRealMs() - g_check_t0;
    bool flip_busy = IntelAccel::FlipActive() && IntelAccel::FlipBusy();
    bool ready = el >= g_check_settle && g_check_frames >= 2 && !flip_busy && !scene_moving();
    if (!ready && el < 8000) return;
    do_compare(g_check_name, !ready);
    g_checking = false;
    g_frozen = false;
    if (g_pc >= NOPS) {
        g_loops++;
        uint64_t freeb = PMM::GetFreeMemory();
        uint32_t now = Timer::GetRealMs();
        SerialLogger::Log("[oracle] loop "); SerialLogger::LogDec((int)g_loops);
        logu(" checkpoints=", g_cp_n); logu(" front_mismatching=", g_cp_bad_front); logu(" back_mismatching=", g_cp_bad_back);
        logu(" pmm_free_kb=", freeb / 1024u); logu(" elapsed_s=", (now - g_soak_t0) / 1000u);
        logu(" frames=", g_frames_total);
        SerialLogger::Log("\r\n");
        if (g_soak_ms && now - g_soak_t0 < g_soak_ms) {
            g_pc = 1; g_op_started = false;      // run the scenario again, windows are made again (satoru)
        } else {
            g_done = true; summary();
        }
    }
}

}  // namespace GfxOracle

// end (satoru)
