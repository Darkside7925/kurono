//  kurono os - settings module: displays (gnome style multi-monitor page) (satoru)
//  an arrangement canvas (drag tiles, edge snapping, click to select), identify,
//  join / mirror / single display modes, per display on-off, primary, resolution,
//  refresh and scale, all staged behind apply / revert with a 15 s "keep these
//  settings?" countdown. the arrangement, primary and on-off persist in ui.conf
//  and are re-applied at boot when the same outputs are present. the legacy
//  frame-rate cap, vsync and brightness controls stay at the bottom and apply
//  live like before. (satoru)
//
//  backend facts the page respects (satoru):
//   - the primary output always sits at the virtual origin (the menu bar, dock,
//     desktop and lock screen are laid out at 0,0), so other displays can only
//     sit to the right of or below it.
//   - firmware (gop) framebuffers are fixed-mode and the intel pipe b mode is
//     picked when the port is lit, so with several outputs resolution and
//     refresh are shown read-only. one output keeps the bga / virtio selector.
//   - mirror is real only when every output has the same physical size (all
//     outputs then scan out the same rect of the virtual desktop). (satoru)
#include "system_settings.h"
#include "settings_display.h"
#include "../drivers/graphics.h"
#include "../drivers/display_mgr.h"
#include "../drivers/outputs.h"
#include "../drivers/intel_modeset.h"
#include "../drivers/mouse.h"
#include "../drivers/timer.h"
#include "../drivers/serial.h"
#include "../system/ui_config.h"
#include "../ui/frame_pacer.h"
#include "../ui/night_light.h"   // night light card (satoru)
#include "../ui/modern_draw.h"
#include "../ui/font.h"

// ── palette: follows the settings theme (light / dark) through SettingsUI::T() (satoru)
#define C_CARD      (SettingsUI::T().card)
#define C_CARD_EDGE (SettingsUI::T().card_line)
#define C_SEP       (SettingsUI::T().sep)
#define C_CANVAS    (SettingsUI::T().dark ? 0xFF141416u : 0xFFE6E6EBu)
#define C_TILE_T    (SettingsUI::T().dark ? 0xFF4A4A52u : 0xFF9A9EAAu)
#define C_TILE_B    (SettingsUI::T().dark ? 0xFF3A3A42u : 0xFF868A96u)
#define C_BTN       (SettingsUI::T().field)
#define C_BTN_HI    (SettingsUI::T().dark ? 0xFF46464Eu : 0xFFFFFFFFu)
#define C_FG        (SettingsUI::T().text)

enum { MODE_JOIN = 0, MODE_MIRROR = 1, MODE_SINGLE = 2 };

// ── session model (constant-initialised statics, ctor-free) (satoru) ──────
struct Disp { char name[16]; int num; int pw, ph; bool intel; };
struct Cfg  { bool on[OUTPUT_MAX]; int ax[OUTPUT_MAX], ay[OUTPUT_MAX]; int primary; int res; int rw, rh; int scale; int hz; };

static Disp s_d[OUTPUT_MAX];
static int  s_n = 0;              // displays this session (satoru)
static bool s_multi = false;      // Graphics::MultiActive at snapshot (satoru)
static Cfg  s_live, s_st, s_prev; // live, staged, pre-apply (satoru)
static int  s_sel = 0;            // selected display (satoru)
static int  s_resync = 0;         // renders left until the live config is re-read (satoru)

static int      s_brightness = 75;
static int      s_refresh_mode = 0;
static bool     s_vsync = true;
static int      s_mode_count = 0;
static DisplayMode s_modes[DISPLAY_MODE_COUNT];
static const int kRates[]   = {0, 60, 120, 144, 165, 240};
static const int kRateCount = (int)(sizeof(kRates) / sizeof(kRates[0]));
static const int kScales[]  = {100, 125, 150, 200};
static const int kScaleCount = 4;

// drag + animation state (satoru)
static int      s_drag = -1;          // display being dragged (satoru)
static bool     s_drag_moved = false;
static int      s_grab_x = 0, s_grab_y = 0, s_press_x = 0, s_press_y = 0;
static int      s_drag_fx = 0, s_drag_fy = 0;   // free model position while dragging (satoru)
static bool     s_frozen = false;
static float    s_fcs = 0.1f; static int s_fox = 0, s_foy = 0;
static int      s_an_x[OUTPUT_MAX], s_an_y[OUTPUT_MAX];   // tween start (model px) (satoru)
static uint32_t s_an_t0 = 0;
static const uint32_t ANIM_MS = 220;
static int      s_draw_x[OUTPUT_MAX], s_draw_y[OUTPUT_MAX]; // last drawn model pos (satoru)
static float    s_cs_from = 0.0f, s_cs_to = 0.0f; static uint32_t s_cs_t0 = 0;

static uint32_t s_ident_ms = 0;       // identify overlay start (satoru)
static uint32_t s_keep_ms = 0;        // keep-dialog start, 0 = closed (satoru)
static bool     s_reopen = false;     // a mode change closes every window: bring settings back (satoru)
static uint32_t s_reopen_ms = 0;
static uint32_t s_res_fail_ms = 0;    // the backend refused the staged mode (satoru)
static const uint32_t KEEP_MS = 15000;
static uint32_t s_bar_ms = 0;         // action bar appeared (satoru)
static bool     s_bar_was = false;
static uint32_t s_note_ms = 0;        // "at least one display" flash (satoru)
static int      s_content_h = 900;
static int      s_last_h = 480, s_last_w = 540;

// pane-local hit rects rebuilt by every render so input matches the drawing. (satoru)
enum {
    H_IDENT = 1, H_MODE0, H_MODE1, H_MODE2, H_PRIMARY, H_PILL0, H_PILL1, H_PILL2, H_PILL3,
    H_ENABLE, H_RES, H_SCALE, H_RATE, H_VSYNC, H_BRIGHT, H_APPLY, H_REVERT, H_KEEP, H_UNDO,
    H_NIGHT, H_NSCHED, H_NSTR,   // night light card (satoru)
    H_DPRATE, H_TILE0, H_TILE1, H_TILE2, H_TILE3
};
struct Hot { int id, x, y, w, h; };
static Hot s_hot[40];
static int s_hot_n = 0;
static int s_px = 0, s_py = 0;      // screen origin of the pane (satoru)
static int s_clip_y0 = 0, s_clip_y1 = 0;

static void hot(int id, int sx, int sy, int w, int h){
    if(s_hot_n >= 40) return;
    s_hot[s_hot_n].id = id; s_hot[s_hot_n].x = sx - s_px; s_hot[s_hot_n].y = sy - s_py;
    s_hot[s_hot_n].w = w; s_hot[s_hot_n].h = h; s_hot_n++;
}
static const Hot* hot_at(int mx, int my, bool tiles_only){
    // last registered wins (the dialog and the action bar are drawn last). (satoru)
    for(int i = s_hot_n - 1; i >= 0; i--){
        const Hot& h = s_hot[i];
        if(tiles_only && h.id < H_TILE0) continue;
        if(mx >= h.x && mx < h.x + h.w && my >= h.y && my < h.y + h.h) return &h;
    }
    return nullptr;
}

// ── small helpers (satoru) ─────────────────────────────────────────────────
static int imin(int a, int b){ return a < b ? a : b; }
static int imax(int a, int b){ return a > b ? a : b; }
static int iabs(int a){ return a < 0 ? -a : a; }
static bool s_eq(const char* a, const char* b){ while(*a && *a == *b){ a++; b++; } return *a == *b; }

static float ease(uint32_t el, uint32_t dur){
    if(el >= dur) return 1.0f;
    float t = (float)el / (float)dur, u = 1.0f - t;
    return 1.0f - u * u * u;     // out cubic (satoru)
}
static bool anim_on(){ return UIConfig::Bool("compositor.window_animations", true); }

// MDraw writes straight to the frame (screen clip only), so it is used only
// when the shape sits fully inside the visible pane; otherwise the clipped
// classic primitive draws it. (satoru)
static int s_clip_x0 = -100000, s_clip_x1 = 100000;
static bool inside(int y, int h){ return y >= s_clip_y0 && y + h <= s_clip_y1; }
static bool inside_x(int x, int w){ return x >= s_clip_x0 && x + w <= s_clip_x1; }
static void rr(int x, int y, int w, int h, int r, uint32_t top, uint32_t bot, int a = 255){
    if(w <= 0 || h <= 0) return;
    if(inside(y, h) && inside_x(x, w)) MDraw::RoundRect(x, y, w, h, r, top, bot, a);
    else if(y + h > s_clip_y0 && y < s_clip_y1) Graphics::FillRoundedRect(x, y, w, h, r, top);
}
static void rr_edge(int x, int y, int w, int h, int r, uint32_t c, int a = 255){
    if(inside(y, h) && inside_x(x, w)) MDraw::RoundOutline(x, y, w, h, r, c, a);
}
static void shadow(int x, int y, int w, int h, int r, int spread, int dy, int a){
    if(inside(y - spread, h + 2 * spread + dy) && inside_x(x - spread, w + 2 * spread)) MDraw::Shadow(x, y, w, h, r, spread, dy, a);
}
static int txt(int x, int y, float px, const char* s, uint32_t c, bool bold = false){
    if(FontTTF::ok) return FontTTF::DrawText(x, y, px, s, c, bold);
    Graphics::DrawString(x, y, s, c, 0xFF000000);
    return SettingsUI::StrLen(s) * 8;
}
static int txt_w(float px, const char* s, bool bold = false){
    if(FontTTF::ok) return FontTTF::MeasureText(px, s, bold);
    return SettingsUI::StrLen(s) * 8;
}
// text sizes follow the primary screen, not the (possibly stacked) virtual desktop (satoru)
static int ref_h(){
    if(Outputs::Multi()){ const Output* o = Outputs::Get(0); if(o && o->h > 0) return o->h; }
    return (int)Graphics::GetHeight();
}
static float body_px(){ return ref_h() >= 1440 ? 20.0f : 15.0f; }
static float small_px(){ return ref_h() >= 1440 ? 16.0f : 12.5f; }

static void res_str(int w, int h, char* b, int mx){
    SettingsUI::IntToStr(w, b, mx); SettingsUI::StrApp(b, " x ", mx);
    char t[12]; SettingsUI::IntToStr(h, t, 12); SettingsUI::StrApp(b, t, mx);
}

// a gnome-style button: rounded pill, accent when primary. (satoru)
static void button(int id, int x, int y, int w, int h, const char* label, bool accent, bool enabled = true){
    uint32_t c = accent ? SettingsUI::Accent() : C_BTN;
    if(!enabled) c = MDraw::Mix(c, C_CARD, 150);
    uint32_t top = MDraw::Mix(c, 0xFFFFFFFF, 18);
    rr(x, y, w, h, h / 2 < 9 ? h / 2 : 9, top, c);
    float px = body_px();
    int tw = txt_w(px, label, true);
    txt(x + (w - tw) / 2, y + (h - (int)px) / 2 - 1, px, label, enabled ? (accent ? SettingsUI::T().on_accent : C_FG) : SettingsUI::T().text3, true);
    if(enabled) hot(id, x, y, w, h);
}

// ── display list + live configuration (satoru) ─────────────────────────────
static int parse_num(const char* name, int fallback){
    int n = 0, i = 0, v = 0; bool any = false;
    while(name[n]) n++;
    i = n;
    while(i > 0 && name[i - 1] >= '0' && name[i - 1] <= '9') i--;
    for(int k = i; k < n; k++){ v = v * 10 + (name[k] - '0'); any = true; }
    return any ? v : fallback;
}

static int slot_of(int d){
    for(int i = 0; i < Outputs::Total(); i++){
        const Output* o = Outputs::Get(i);
        if(o && o->pw == s_d[d].pw && o->ph == s_d[d].ph && s_eq(o->name, s_d[d].name)) return i;
    }
    return -1;
}

static bool switchable_res(){
    if(s_multi || s_mode_count <= 0) return false;
    DisplayBackend be = DisplayManager::GetBackend();
    return be == DISPLAY_BACKEND_BGA || be == DISPLAY_BACKEND_VIRTIO_GPU;
}

static int current_mode_index(){
    uint32_t cw = DisplayManager::GetWidth(), ch = DisplayManager::GetHeight();
    for(int i = 0; i < s_mode_count; i++)
        if(s_modes[i].width == cw && s_modes[i].height == ch) return i;
    return -1;
}

static void build_displays(){
    s_multi = Graphics::MultiActive() && Outputs::Total() >= 2;
    if(s_multi){
        // keep the session order stable unless the output set changed (satoru)
        bool same = (s_n == Outputs::Total());
        for(int d = 0; same && d < s_n; d++) if(slot_of(d) < 0) same = false;
        if(same) return;
        uint64_t ip = 0; int iw = 0, ih = 0; uint32_t ipt = 0;
        bool have_intel = IntelModeset::GetOutput(&ip, &iw, &ih, &ipt);
        s_n = Outputs::Total();
        for(int i = 0; i < s_n; i++){
            const Output* o = Outputs::Get(i);
            SettingsUI::StrCpy(s_d[i].name, o->name, 16);
            s_d[i].num = parse_num(o->name, i + 1);
            s_d[i].pw = o->pw; s_d[i].ph = o->ph;
            s_d[i].intel = have_intel && o->phys == ip;
        }
        if(s_sel >= s_n) s_sel = 0;
        return;
    }
    s_n = 1;
    const MonitorInfo& mon = DisplayManager::GetMonitorInfo();
    SettingsUI::StrCpy(s_d[0].name, (mon.connected && mon.name[0]) ? mon.name : "Built-in Display", 16);
    s_d[0].num = 1;
    s_d[0].pw = (int)DisplayManager::GetWidth();
    s_d[0].ph = (int)DisplayManager::GetHeight();
    if(s_d[0].pw <= 0 || s_d[0].ph <= 0){
        s_d[0].pw = (int)Graphics::GetWidth() * Graphics::GetUIScale() / 100;
        s_d[0].ph = (int)Graphics::GetHeight() * Graphics::GetUIScale() / 100;
    }
    s_d[0].intel = false;
    s_sel = 0;
}

static void read_live(Cfg& c){
    for(int d = 0; d < OUTPUT_MAX; d++){ c.on[d] = false; c.ax[d] = 0; c.ay[d] = 0; }
    c.primary = 0;
    if(s_multi){
        for(int d = 0; d < s_n; d++){
            int s = slot_of(d);
            const Output* o = Outputs::Get(s);
            if(!o) continue;
            c.on[d] = o->connected; c.ax[d] = o->ax; c.ay[d] = o->ay;
            if(s == 0) c.primary = d;
        }
        int bx = c.ax[c.primary], by = c.ay[c.primary];
        for(int d = 0; d < s_n; d++){ c.ax[d] -= bx; c.ay[d] -= by; }
    } else {
        c.on[0] = true;
    }
    c.res = switchable_res() ? current_mode_index() : -1;
    c.rw = s_multi ? 0 : (int)DisplayManager::GetWidth();
    c.rh = s_multi ? 0 : (int)DisplayManager::GetHeight();
    c.scale = Graphics::GetUIScale();
    // the external pipe b monitor's hardware rate, when one is lit. (satoru)
    c.hz = (int)IntelModeset::CurrentHz();
}

static bool cfg_same(const Cfg& a, const Cfg& b){
    if(a.primary != b.primary || a.rw != b.rw || a.rh != b.rh || a.scale != b.scale || a.hz != b.hz) return false;
    for(int d = 0; d < s_n; d++){
        if(a.on[d] != b.on[d]) return false;
        if(a.on[d] && (a.ax[d] != b.ax[d] || a.ay[d] != b.ay[d])) return false;
    }
    return true;
}

static int on_count(const Cfg& c){ int n = 0; for(int d = 0; d < s_n; d++) if(c.on[d]) n++; return n; }

static int mode_of(const Cfg& c){
    if(s_n < 2) return MODE_JOIN;
    int n = on_count(c);
    if(n == 1) return MODE_SINGLE;
    for(int d = 0; d < s_n; d++)
        if(c.on[d] && (c.ax[d] != 0 || c.ay[d] != 0)) return MODE_JOIN;
    return MODE_MIRROR;
}

static bool mirror_possible(){
    if(!s_multi || s_n < 2) return false;
    for(int d = 1; d < s_n; d++) if(s_d[d].pw != s_d[0].pw || s_d[d].ph != s_d[0].ph) return false;
    return true;
}

// ── arrangement geometry (model = physical px, primary at 0,0) (satoru) ────
static bool ov(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh){
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}
static bool touch(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh){
    bool vy = ay < by + bh && by < ay + ah;
    bool vx = ax < bx + bw && bx < ax + aw;
    return ((ax == bx + bw || bx == ax + aw) && vy) || ((ay == by + bh || by == ay + ah) && vx);
}
// valid spot for d at (x,y) against the displays in `set`: on-screen of the
// origin, no overlap, and (when need_touch) sharing an edge with one of them. (satoru)
static bool spot_ok(const Cfg& c, int d, int x, int y, const bool* set, bool need_touch){
    if(x < 0 || y < 0) return false;
    bool t = !need_touch;
    for(int e = 0; e < s_n; e++){
        if(e == d || !set[e] || !c.on[e]) continue;
        if(ov(x, y, s_d[d].pw, s_d[d].ph, c.ax[e], c.ay[e], s_d[e].pw, s_d[e].ph)) return false;
        if(touch(x, y, s_d[d].pw, s_d[d].ph, c.ax[e], c.ay[e], s_d[e].pw, s_d[e].ph)) t = true;
    }
    return t;
}
static int align(int v, int lo, int hi, int size, int thr){
    if(iabs(v - lo) <= thr) return lo;
    if(iabs(v + size - hi) <= thr) return hi - size;
    int mid = (lo + hi) / 2 - size / 2;
    if(iabs(v - mid) <= thr) return mid;
    return v;
}
// nearest edge-to-edge spot for d around the displays in `set`. returns false
// when there is none (the caller keeps a fallback). (satoru)
// `bias` (px) favours spots whose edge lines up with the neighbour's, so a
// re-attached display lands flush instead of one pixel off. (satoru)
static bool snap_spot(const Cfg& c, int d, int fx, int fy, const bool* set, int thr, int* ox, int* oy, int bias = 0){
    int w = s_d[d].pw, h = s_d[d].ph;
    long long best = -1; int bx = 0, by = 0;
    for(int t = 0; t < s_n; t++){
        if(t == d || !set[t] || !c.on[t]) continue;
        int tx = c.ax[t], ty = c.ay[t], tw = s_d[t].pw, th = s_d[t].ph;
        for(int side = 0; side < 4; side++){
            int x, y;
            if(side < 2){
                x = side == 0 ? tx + tw : tx - w;
                // share at least a quarter of the shorter edge, never a sliver (satoru)
                int mo = imax(imin(h, th) / 4, 1);
                int lo = imax(ty - h + mo, 0), hi = ty + th - mo;
                if(lo > hi) continue;
                y = fy < lo ? lo : (fy > hi ? hi : fy);
                int ya = align(y, ty, ty + th, h, thr);
                if(ya >= lo && ya <= hi) y = ya;
            } else {
                y = side == 2 ? ty + th : ty - h;
                int mo = imax(imin(w, tw) / 4, 1);
                int lo = imax(tx - w + mo, 0), hi = tx + tw - mo;
                if(lo > hi) continue;
                x = fx < lo ? lo : (fx > hi ? hi : fx);
                int xa = align(x, tx, tx + tw, w, thr);
                if(xa >= lo && xa <= hi) x = xa;
            }
            if(!spot_ok(c, d, x, y, set, true)) continue;
            long long dx = x - fx, dy = y - fy, dist = dx * dx + dy * dy;
            bool aligned = side < 2 ? (y == ty || y + h == ty + th) : (x == tx || x + w == tx + tw);
            if(!aligned) dist += (long long)bias * bias;
            if(best < 0 || dist < best){ best = dist; bx = x; by = y; }
        }
    }
    if(best < 0) return false;
    *ox = bx; *oy = by;
    return true;
}
// make the staged layout valid: primary at the origin, every on display edge
// connected to the rest with no gaps or overlaps. `keep` is placed first when it
// is already valid; reflect mirrors displays pushed to negative coordinates
// (a primary change) to the other side instead of stacking them. (satoru)
static void fix_layout(Cfg& c, int keep, bool reflect){
    int p = c.primary;
    int bx = c.ax[p], by = c.ay[p];
    for(int d = 0; d < s_n; d++){ c.ax[d] -= bx; c.ay[d] -= by; }
    if(reflect){
        for(int d = 0; d < s_n; d++){
            if(!c.on[d] || d == p) continue;
            if(c.ax[d] < 0) c.ax[d] = s_d[p].pw + (-(c.ax[d] + s_d[d].pw));
            if(c.ay[d] < 0) c.ay[d] = s_d[p].ph + (-(c.ay[d] + s_d[d].ph));
        }
    }
    bool placed[OUTPUT_MAX] = {false, false, false, false};
    placed[p] = true;
    if(keep >= 0 && keep != p && c.on[keep] && spot_ok(c, keep, c.ax[keep], c.ay[keep], placed, true))
        placed[keep] = true;
    for(int guard = 0; guard < OUTPUT_MAX; guard++){
        int left = -1;
        bool progress = false;
        for(int d = 0; d < s_n; d++){
            if(!c.on[d] || placed[d]) continue;
            if(spot_ok(c, d, c.ax[d], c.ay[d], placed, true)){ placed[d] = true; progress = true; }
            else if(left < 0 || iabs(c.ax[d]) + iabs(c.ay[d]) < iabs(c.ax[left]) + iabs(c.ay[left])) left = d;
        }
        if(progress) continue;
        if(left < 0) break;
        int nx, ny;
        int bias = imin(s_d[left].pw, s_d[left].ph) / 4;
        if(!snap_spot(c, left, c.ax[left], c.ay[left], placed, 0, &nx, &ny, bias)){
            // right of everything placed, at the top of the rightmost one. (satoru)
            int mr = 0, my = 0;
            for(int e = 0; e < s_n; e++) if(placed[e] && c.ax[e] + s_d[e].pw > mr){ mr = c.ax[e] + s_d[e].pw; my = c.ay[e]; }
            nx = mr; ny = my;
        }
        c.ax[left] = nx; c.ay[left] = ny;
        placed[left] = true;
    }
}

// ── animation (satoru) ─────────────────────────────────────────────────────
static void begin_anim(){
    for(int d = 0; d < s_n; d++){ s_an_x[d] = s_draw_x[d]; s_an_y[d] = s_draw_y[d]; }
    s_an_t0 = Timer::GetRealMs();
    if(!anim_on()) s_an_t0 = 0;
}
static bool anim_running(){ return s_an_t0 && (Timer::GetRealMs() - s_an_t0) < ANIM_MS; }

// ── staged edits (satoru) ──────────────────────────────────────────────────
static void place_new(Cfg& c, int d){
    int mr = 0;
    for(int e = 0; e < s_n; e++) if(e != d && c.on[e] && c.ax[e] + s_d[e].pw > mr) mr = c.ax[e] + s_d[e].pw;
    c.ax[d] = mr; c.ay[d] = 0;
}
static void strip(Cfg& c){
    int x = s_d[c.primary].pw;
    c.ax[c.primary] = 0; c.ay[c.primary] = 0;
    for(int d = 0; d < s_n; d++){
        if(d == c.primary) continue;
        c.on[d] = true; c.ax[d] = x; c.ay[d] = 0; x += s_d[d].pw;
    }
    c.on[c.primary] = true;
}
static void set_mode(int m){
    int cur = mode_of(s_st);
    if(m == cur) return;
    if(m == MODE_MIRROR && !mirror_possible()) return;
    begin_anim();
    if(m == MODE_MIRROR){
        for(int d = 0; d < s_n; d++){ s_st.on[d] = true; s_st.ax[d] = 0; s_st.ay[d] = 0; }
    } else if(m == MODE_SINGLE){
        if(!s_st.on[s_sel]) s_sel = s_st.primary;
        s_st.primary = s_sel;
        for(int d = 0; d < s_n; d++) s_st.on[d] = (d == s_st.primary);
        s_st.ax[s_st.primary] = 0; s_st.ay[s_st.primary] = 0;
    } else {
        // join: reuse the live arrangement when it was joined, else a strip (satoru)
        if(mode_of(s_live) == MODE_JOIN && s_live.primary == s_st.primary){
            for(int d = 0; d < s_n; d++){ s_st.on[d] = true; s_st.ax[d] = s_live.ax[d]; s_st.ay[d] = s_live.ay[d]; }
            for(int d = 0; d < s_n; d++) if(!s_live.on[d]) place_new(s_st, d);
            fix_layout(s_st, -1, false);
        } else {
            strip(s_st);
        }
    }
}
static void set_primary(int d){
    if(d == s_st.primary || !s_st.on[d]) return;
    begin_anim();
    bool mirror = mode_of(s_st) == MODE_MIRROR;
    s_st.primary = d;
    if(!mirror) fix_layout(s_st, -1, true);
}
static void set_enabled(int d, bool on){
    if(s_st.on[d] == on) return;
    if(!on && on_count(s_st) <= 1){ s_note_ms = Timer::GetRealMs(); return; }
    begin_anim();
    bool mirror = mode_of(s_st) == MODE_MIRROR;
    s_st.on[d] = on;
    if(on){
        if(mirror){ s_st.ax[d] = 0; s_st.ay[d] = 0; }
        else { place_new(s_st, d); fix_layout(s_st, d, false); }
    } else {
        if(d == s_st.primary){
            for(int e = 0; e < s_n; e++) if(s_st.on[e]){ s_st.primary = e; break; }
        }
        if(!mirror || on_count(s_st) == 1) fix_layout(s_st, -1, true);
    }
}

// ── commit / revert / persist (satoru) ─────────────────────────────────────
// returns false when the backend refused the mode change (nothing else is
// committed then). (satoru)
static bool commit(const Cfg& c){
    // the external monitor's hardware refresh: a real pipe b off/on through
    // intel_modeset, before the layout is touched. a refused timing leaves
    // the previous one lit and nothing else is committed. (satoru)
    if(c.hz > 0 && c.hz != s_live.hz){
        char msg[128];
        if(!IntelModeset::SetRate(msg, (int)sizeof(msg), (uint32_t)c.hz)){
            Graphics::MarkUIDirty();
            SerialLogger::Log("[displays] the monitor refused the staged refresh rate\r\n");
            return false;
        }
        FramePacer::OutputsChanged();
        SerialLogger::Log("[displays] monitor refresh now ");
        SerialLogger::LogDec((int)IntelModeset::CurrentHz());
        SerialLogger::Log(" Hz\r\n");
    }
    if(s_multi){
        bool on[OUTPUT_MAX] = {false, false, false, false};
        int ax[OUTPUT_MAX] = {0, 0, 0, 0}, ay[OUTPUT_MAX] = {0, 0, 0, 0};
        for(int d = 0; d < s_n; d++){
            int s = slot_of(d);
            if(s < 0) continue;
            on[s] = c.on[d]; ax[s] = c.ax[d]; ay[s] = c.ay[d];
        }
        int ps = slot_of(c.primary);
        if(ps >= 0) Graphics::RequestOutputConfig(ps, on, ax, ay);
    }
    if(!s_multi && c.rw > 0 && c.rh > 0 && (c.rw != s_live.rw || c.rh != s_live.rh)){
        // a mode change re-lays the desktop out and closes every window, so the
        // overlay hook re-opens settings for the keep dialog. (satoru)
        uint8_t bpp = (c.res >= 0 && c.res < s_mode_count && s_modes[c.res].bpp) ? s_modes[c.res].bpp : 32;
        if(!DisplayManager::SetResolution((uint32_t)c.rw, (uint32_t)c.rh, bpp)){
            // the old mode stays; vram was cleared by the attempt, so repaint all (satoru)
            Graphics::MarkDirty(0, 0, (int)Graphics::GetWidth(), (int)Graphics::GetHeight());
            Graphics::MarkUIDirty();
            SerialLogger::Log("[displays] the backend refused the staged mode\r\n");
            return false;
        }
        s_reopen = true;
        s_reopen_ms = Timer::GetRealMs();
    }
    if(c.scale != s_live.scale) Graphics::RequestUIScale(c.scale);
    s_live = c;
    s_resync = 2;      // re-read once the gui loop applied the op (satoru)
    Graphics::MarkUIDirty();
    return true;
}

static void app_int(char* b, int v, int mx){ char t[12]; SettingsUI::IntToStr(v, t, 12); SettingsUI::StrApp(b, t, mx); }

static void persist(const Cfg& c){
    UIConfig::SetInt("display.ui_scale", c.scale, true);
    if(c.hz > 0){
        // save what the pipe really locked, not the staged value: a timing the
        // monitor refused falls back to the previous one. (satoru)
        int now_hz = (int)IntelModeset::CurrentHz();
        UIConfig::SetInt("display.dp_hz", now_hz > 0 ? now_hz : c.hz, true);
    }
    if(s_multi){
        // slot order after the apply: the primary first, then the rest. (satoru)
        UIConfig::SetInt("display.outputs", s_n, true);
        int k = 0;
        for(int pass = 0; pass < 2; pass++){
            for(int d = 0; d < s_n; d++){
                if((pass == 0) != (d == c.primary)) continue;
                char key[20] = "display.out";
                app_int(key, k, 20);
                char v[48]; v[0] = 0;
                SettingsUI::StrApp(v, s_d[d].name, 48); SettingsUI::StrApp(v, ",", 48);
                app_int(v, s_d[d].pw, 48); SettingsUI::StrApp(v, ",", 48);
                app_int(v, s_d[d].ph, 48); SettingsUI::StrApp(v, ",", 48);
                app_int(v, c.on[d] ? 1 : 0, 48); SettingsUI::StrApp(v, ",", 48);
                app_int(v, c.ax[d], 48); SettingsUI::StrApp(v, ",", 48);
                app_int(v, c.ay[d], 48);
                UIConfig::Set(key, v, true);
                k++;
            }
        }
    }
    UIConfig::Save();
    SerialLogger::Log("[displays] configuration kept and saved\r\n");
}

static bool geometry_changed(const Cfg& a, const Cfg& b){ return !cfg_same(a, b); }

static void do_apply(){
    if(cfg_same(s_st, s_live)) return;
    s_prev = s_live;
    // a hardware refresh change is a visible, revertable change like a mode
    // switch: show the keep dialog and time it out back to the old rate. (satoru)
    bool geo = geometry_changed(s_st, s_live) || s_st.hz != s_live.hz;
    if(!commit(s_st)){
        s_st.rw = s_live.rw; s_st.rh = s_live.rh; s_st.res = s_live.res;
        if(s_st.rw > 0){ s_d[0].pw = s_st.rw; s_d[0].ph = s_st.rh; }
        s_res_fail_ms = Timer::GetRealMs();
        return;
    }
    SerialLogger::Log("[displays] applied staged configuration\r\n");
    if(geo){ s_keep_ms = Timer::GetRealMs(); if(!s_keep_ms) s_keep_ms = 1; }
    else persist(s_live);
}
static void do_keep(){
    s_keep_ms = 0;
    persist(s_live);
    Graphics::MarkUIDirty();
}
static void do_undo(){
    s_keep_ms = 0;
    begin_anim();
    commit(s_prev);
    s_st = s_prev;
    if(!s_multi && s_st.rw > 0){ s_d[0].pw = s_st.rw; s_d[0].ph = s_st.rh; }
    SerialLogger::Log("[displays] reverted to the previous configuration\r\n");
}

// ── legacy live controls (unchanged behaviour) (satoru) ───────────────────
static void persist_brightness(){
    UIConfig::SetInt("display.brightness", s_brightness, true);
    DisplayManager::SetBrightness((uint8_t)((s_brightness * 255) / 100));
    Graphics::SetBrightness(s_brightness);
    UIConfig::Save();
}
static void persist_vsync(){
    UIConfig::SetInt("display.vsync", s_vsync ? 1 : 0, true);
    DisplayManager::SetVSync(s_vsync ? VSYNC_ON : VSYNC_OFF);
    UIConfig::Save();
}

static uint32_t s_live_sig = 0;
static uint32_t live_sig();

// ── on_show (satoru) ────────────────────────────────────────────────────────
static void display_on_show(){
    int count = 0;
    if(DisplayManager::GetSupportedModes(s_modes, DISPLAY_MODE_COUNT, &count) && count > 0) s_mode_count = count;
    else s_mode_count = 0;
    s_brightness = UIConfig::Int("display.brightness", s_brightness);
    if(s_brightness < 0) s_brightness = 0;
    if(s_brightness > 100) s_brightness = 100;
    s_refresh_mode = (int)FramePacer::Mode();
    s_vsync = UIConfig::Int("display.vsync", s_vsync ? 1 : 0) != 0;
    build_displays();
    if(!s_keep_ms){
        read_live(s_live);
        s_st = s_live;
    }
    for(int d = 0; d < s_n; d++){ s_draw_x[d] = s_st.ax[d]; s_draw_y[d] = s_st.ay[d]; }
    s_an_t0 = 0; s_drag = -1; s_frozen = false; s_cs_to = 0.0f;
    s_live_sig = live_sig();
}

// ── render pieces (satoru) ─────────────────────────────────────────────────
static void card(int x, int y, int w, int h){
    rr(x, y, w, h, 12, C_CARD, C_CARD);
    rr_edge(x, y, w, h, 12, C_CARD_EDGE, 255);
}
static void card_title(int x, int y, const char* t){
    txt(x + 2, y, small_px() + 0.5f, t, SettingsUI::COL_DIM, true);
}
static void row_sep(int x, int y, int w){
    if(y >= s_clip_y0 && y < s_clip_y1) Graphics::FillRect(x + 14, y, w - 28, 1, C_SEP);
}
static void row_label(int x, int y, int rh, const char* label, const char* sub){
    float px = body_px();
    if(sub){
        txt(x + 16, y + rh / 2 - (int)px + 1, px, label, SettingsUI::COL_TEXT);
        txt(x + 16, y + rh / 2 + 3, small_px(), sub, SettingsUI::COL_DIM);
    } else {
        txt(x + 16, y + (rh - (int)px) / 2 - 1, px, label, SettingsUI::COL_TEXT);
    }
}
static void row_value(int x, int y, int w, int rh, const char* v){
    float px = body_px();
    int tw = txt_w(px, v);
    txt(x + w - 16 - tw, y + (rh - (int)px) / 2 - 1, px, v, SettingsUI::COL_DIM);
}

static void badge(int x, int y, int sz, int num, uint32_t bg, uint32_t fg){
    rr(x, y, sz, sz, sz / 4 + 2, bg, bg);
    char b[4]; SettingsUI::IntToStr(num, b, 4);
    float px = (float)sz * 0.66f;
    int tw = txt_w(px, b, true);
    txt(x + (sz - tw) / 2, y + (sz - (int)px) / 2 - 1, px, b, fg, true);
}

static void draw_tile(int d, int tx, int ty, int tw, int th, bool sel, bool primary, bool lifted, bool mirror){
    uint32_t acc = SettingsUI::Accent();
    uint32_t top = sel ? MDraw::Mix(acc, 0xFFFFFFFF, 40) : C_TILE_T;
    uint32_t bot = sel ? acc : C_TILE_B;
    if(mirror){
        // a second card peeking out behind = mirrored (satoru)
        rr(tx + 7, ty + 7, tw, th, 8, MDraw::Mix(top, C_CANVAS, 110), MDraw::Mix(bot, C_CANVAS, 110));
    }
    shadow(tx, ty, tw, th, 8, lifted ? 14 : 6, lifted ? 6 : 2, lifted ? 120 : 70);
    rr(tx, ty, tw, th, 8, top, bot, lifted ? 230 : 255);
    rr_edge(tx, ty, tw, th, 8, sel ? MDraw::Mix(acc, 0xFFFFFFFF, 90) : 0xFF5A5A62, 255);
    if(primary && th > 26){
        // the menu bar strip marks the primary like gnome's top bar (satoru)
        if(inside(ty + 1, 6) && inside_x(tx, tw)) MDraw::RoundRect(tx + 1, ty + 1, tw - 2, 6, 3, 0xFF101012, 200);
    }
    int bs = th >= 70 ? 24 : 18;
    if(mirror && s_n >= 2){
        int bx = tx + 10;
        for(int e = 0; e < s_n; e++){
            if(!s_st.on[e]) continue;
            badge(bx, ty + 14, bs, s_d[e].num, 0xFFF2F2F4, 0xFF1B1B1D);
            bx += bs + 6;
        }
    } else {
        badge(tx + 10, ty + 14, bs, s_d[d].num, 0xFFF2F2F4, 0xFF1B1B1D);
    }
    if(th >= 56){
        const char* nm = mirror ? "Mirrored" : s_d[d].name;
        float px = small_px() + 0.5f;
        int nw = txt_w(px, nm, true);
        if(nw < tw - 12) txt(tx + (tw - nw) / 2, ty + th - (int)px - 12, px, nm, 0xFFFFFFFF, true);
    }
}

static void render_canvas(int x, int y, int w, int ch){
    card(x, y, w, ch);
    rr(x + 1, y + 1, w - 2, ch - 2, 11, C_CANVAS, C_CANVAS);
    int mode = mode_of(s_st);
    uint32_t now = Timer::GetRealMs();

    // displays in the canvas (satoru)
    bool vis[OUTPUT_MAX] = {false, false, false, false};
    for(int d = 0; d < s_n; d++) vis[d] = s_st.on[d];
    if(mode == MODE_MIRROR){ for(int d = 0; d < s_n; d++) vis[d] = (d == s_st.primary); }

    // animated model positions (satoru)
    float e = anim_running() ? ease(now - s_an_t0, ANIM_MS) : 1.0f;
    for(int d = 0; d < s_n; d++){
        int tx = s_st.ax[d], ty = s_st.ay[d];
        if(e < 1.0f){
            tx = s_an_x[d] + (int)((float)(tx - s_an_x[d]) * e);
            ty = s_an_y[d] + (int)((float)(ty - s_an_y[d]) * e);
        }
        s_draw_x[d] = tx; s_draw_y[d] = ty;
    }

    // fit: bounding box of the staged layout, eased when it changes (satoru)
    int bx0 = 1 << 30, by0 = 1 << 30, bx1 = -(1 << 30), by1 = -(1 << 30);
    for(int d = 0; d < s_n; d++){
        if(!vis[d]) continue;
        bx0 = imin(bx0, s_st.ax[d]); by0 = imin(by0, s_st.ay[d]);
        bx1 = imax(bx1, s_st.ax[d] + s_d[d].pw); by1 = imax(by1, s_st.ay[d] + s_d[d].ph);
    }
    if(bx1 <= bx0){ bx0 = 0; by0 = 0; bx1 = 1920; by1 = 1080; }
    int pad_x = 36, pad_top = s_multi ? 44 : 26, pad_bot = s_multi ? 34 : 26;
    int aw = w - 2 * pad_x, ah = ch - pad_top - pad_bot;
    float cs = (float)aw / (float)(bx1 - bx0);
    float cs2 = (float)ah / (float)(by1 - by0);
    if(cs2 < cs) cs = cs2;
    if(cs > 0.22f) cs = 0.22f;
    int ox, oy;
    if(s_frozen){ cs = s_fcs; ox = s_fox; oy = s_foy; }
    else {
        if(s_cs_to == 0.0f){ s_cs_to = cs; s_cs_from = cs; s_cs_t0 = 0; }
        else if(cs != s_cs_to){
            float live = s_cs_t0 ? s_cs_from + (s_cs_to - s_cs_from) * ease(now - s_cs_t0, ANIM_MS) : s_cs_to;
            s_cs_from = live; s_cs_to = cs; s_cs_t0 = anim_on() ? now : 0;
        }
        if(s_cs_t0 && now - s_cs_t0 < ANIM_MS){ cs = s_cs_from + (s_cs_to - s_cs_from) * ease(now - s_cs_t0, ANIM_MS); Graphics::MarkUIDirty(); }
        ox = x + pad_x + (aw - (int)((float)(bx1 - bx0) * cs)) / 2 - (int)((float)bx0 * cs);
        oy = y + pad_top + (ah - (int)((float)(by1 - by0) * cs)) / 2 - (int)((float)by0 * cs);
    }

    // drag tracking: the button is polled here because the window manager only
    // forwards the press (event 1) to settings. (satoru)
    if(s_drag >= 0){
        int mx = Mouse::mx, my = Mouse::my;
        if(Mouse::IsLeftDown()){
            if(iabs(mx - s_press_x) > 3 || iabs(my - s_press_y) > 3) s_drag_moved = true;
            int tw = (int)((float)s_d[s_drag].pw * cs), th = (int)((float)s_d[s_drag].ph * cs);
            int sx = mx - s_grab_x, sy = my - s_grab_y;
            if(sx < x + 6) sx = x + 6;
            if(sx + tw > x + w - 6) sx = x + w - 6 - tw;
            if(sy < y + 6) sy = y + 6;
            if(sy + th > y + ch - 6) sy = y + ch - 6 - th;
            s_drag_fx = (int)((float)(sx - ox) / cs);
            s_drag_fy = (int)((float)(sy - oy) / cs);
            Graphics::MarkUIDirty();
        } else {
            int d = s_drag;
            s_drag = -1; s_frozen = false;
            if(s_drag_moved && d != s_st.primary){
                bool set[OUTPUT_MAX] = {true, true, true, true};
                int thr = (int)(10.0f / cs);
                int nx, ny;
                begin_anim();
                s_an_x[d] = s_drag_fx; s_an_y[d] = s_drag_fy;
                if(!s_an_t0){ s_an_t0 = 0; }
                if(snap_spot(s_st, d, s_drag_fx, s_drag_fy, set, thr, &nx, &ny)){
                    s_st.ax[d] = nx; s_st.ay[d] = ny;
                }
                fix_layout(s_st, d, false);
                if(anim_on()) s_an_t0 = Timer::GetRealMs();
            }
            Graphics::MarkUIDirty();
        }
    }

    // drop preview: where the tile would land (satoru)
    int pv_x = 0, pv_y = 0; bool preview = false;
    if(s_drag >= 0 && s_drag_moved && s_drag != s_st.primary){
        bool set[OUTPUT_MAX] = {true, true, true, true};
        preview = snap_spot(s_st, s_drag, s_drag_fx, s_drag_fy, set, (int)(10.0f / cs), &pv_x, &pv_y);
    }

    // draw: unselected first, then selected, the dragged tile last. tiles are
    // clipped to the canvas card. (satoru)
    int sy0 = s_clip_y0, sy1 = s_clip_y1;
    if(y + 2 > s_clip_y0) s_clip_y0 = y + 2;
    if(y + ch - 2 < s_clip_y1) s_clip_y1 = y + ch - 2;
    s_clip_x0 = x + 2; s_clip_x1 = x + w - 2;
    Graphics::PushClipRect(x + 2, s_clip_y0, w - 4, s_clip_y1 > s_clip_y0 ? s_clip_y1 - s_clip_y0 : 0);
    for(int pass = 0; pass < 3; pass++){
        for(int d = 0; d < s_n; d++){
            if(!vis[d]) continue;
            bool dragged = (d == s_drag && s_drag_moved && d != s_st.primary);
            bool sel = (d == s_sel) || (mode == MODE_MIRROR);
            int want = dragged ? 2 : (sel ? 1 : 0);
            if(want != pass) continue;
            int mx_ = dragged ? s_drag_fx : s_draw_x[d];
            int my_ = dragged ? s_drag_fy : s_draw_y[d];
            int tx = ox + (int)((float)mx_ * cs), ty = oy + (int)((float)my_ * cs);
            int tw = (int)((float)s_d[d].pw * cs), th = (int)((float)s_d[d].ph * cs);
            if(dragged && preview){
                int px_ = ox + (int)((float)pv_x * cs), py_ = oy + (int)((float)pv_y * cs);
                rr(px_, py_, tw, th, 8, MDraw::Mix(C_CANVAS, SettingsUI::Accent(), 60), MDraw::Mix(C_CANVAS, SettingsUI::Accent(), 40));
                rr_edge(px_, py_, tw, th, 8, SettingsUI::Accent(), 200);
            }
            draw_tile(d, tx, ty, tw, th, sel, d == s_st.primary, dragged, mode == MODE_MIRROR);
            hot(H_TILE0 + d, tx, ty, tw, th);
        }
    }
    Graphics::PopClipRect();
    s_clip_y0 = sy0; s_clip_y1 = sy1;
    s_clip_x0 = -100000; s_clip_x1 = 100000;
    if(anim_running()) Graphics::MarkUIDirty();

    if(s_multi){
        card_title(x + 16, y + 14, "ARRANGEMENT");
        button(H_IDENT, x + w - 104, y + 10, 90, 28, "Identify", false);
        const char* hint = mode == MODE_JOIN
            ? "Drag displays to match your desk. The primary display stays at the top left."
            : (mode == MODE_MIRROR ? "Every display shows the same picture." : "Only one display is in use.");
        float px = small_px();
        int hw = txt_w(px, hint);
        txt(x + (w - hw) / 2, y + ch - (int)px - 12, px, hint, SettingsUI::COL_DIM);
    }
}

static void segmented(int x, int y, int w, int h, int active, bool mirror_ok){
    static const char* labels[3] = {"Join Displays", "Mirror", "Single Display"};
    rr(x, y, w, h, h / 2, SettingsUI::T().field, SettingsUI::T().field);
    rr_edge(x, y, w, h, h / 2, C_CARD_EDGE, 255);
    int sw = w / 3;
    for(int i = 0; i < 3; i++){
        int sx = x + i * sw;
        bool en = (i != MODE_MIRROR) || mirror_ok;
        if(i == active) rr(sx + 3, y + 3, sw - 6, h - 6, (h - 6) / 2, C_BTN_HI, C_BTN);
        float px = body_px();
        int tw = txt_w(px, labels[i], i == active);
        uint32_t c = i == active ? C_FG : (en ? SettingsUI::COL_TEXT : SettingsUI::T().text3);
        txt(sx + (sw - tw) / 2, y + (h - (int)px) / 2 - 1, px, labels[i], c, i == active);
        if(en) hot(H_MODE0 + i, sx, y, sw, h);
    }
}

static void pills(int x, int y, int w, int h){
    int n = s_n, gap = 8;
    int pw = (w - gap * (n - 1)) / n;
    for(int d = 0; d < n; d++){
        int px_ = x + d * (pw + gap);
        bool sel = (d == s_sel);
        uint32_t bg = sel ? MDraw::Mix(C_CARD, SettingsUI::Accent(), 70) : C_CARD;
        rr(px_, y, pw, h, 10, bg, bg);
        rr_edge(px_, y, pw, h, 10, sel ? SettingsUI::Accent() : C_CARD_EDGE, 255);
        badge(px_ + 10, y + (h - 22) / 2, 22, s_d[d].num, s_st.on[d] ? 0xFFF2F2F4 : 0xFF55555B, 0xFF1B1B1D);
        float fp = body_px();
        txt(px_ + 40, y + 7, fp, s_d[d].name, SettingsUI::COL_TEXT, sel);
        char sub[40];
        if(!s_st.on[d]) SettingsUI::StrCpy(sub, "Off", 40);
        else {
            res_str(s_d[d].pw, s_d[d].ph, sub, 40);
            if(d == s_st.primary) SettingsUI::StrApp(sub, "  Primary", 40);
        }
        txt(px_ + 40, y + 9 + (int)fp, small_px(), sub, SettingsUI::COL_DIM);
        hot(H_PILL0 + d, px_, y, pw, h);
    }
}

static void dropdown_row(int id, int x, int y, int w, int rh, const char* value){
    int dw = 220;
    if(dw > w / 2) dw = w / 2;
    int dx = x + w - 16 - dw, dy = y + (rh - SettingsUI::DROPDOWN_H) / 2;
    SettingsUI::Dropdown(dx, dy, dw, value);
    hot(id, dx, dy, dw, SettingsUI::DROPDOWN_H);
}

static void render_action_bar(int x, int y, int w, int h){
    bool dirty = !s_keep_ms && !cfg_same(s_st, s_live);
    uint32_t now = Timer::GetRealMs();
    if(dirty && !s_bar_was){ s_bar_ms = anim_on() ? now : 0; }
    s_bar_was = dirty;
    if(!dirty) return;
    float e = s_bar_ms ? ease(now - s_bar_ms, 220) : 1.0f;
    if(e < 1.0f) Graphics::MarkUIDirty();
    int bh = 56;
    int by = y + h - (int)((float)(bh + 8) * e);
    int bx = x - 8, bw = w + 0;
    shadow(bx, by, bw, bh, 14, 10, 2, 110);
    rr(bx, by, bw, bh, 14, 0xFF2C2C31, 0xFF26262A);
    rr_edge(bx, by, bw, bh, 14, C_CARD_EDGE, 255);
    txt(bx + 18, by + (bh - (int)body_px()) / 2 - 1, body_px(), "Display settings changed", SettingsUI::COL_TEXT, true);
    button(H_APPLY, bx + bw - 14 - 96, by + 12, 96, 32, "Apply", true);
    button(H_REVERT, bx + bw - 14 - 96 - 10 - 96, by + 12, 96, 32, "Revert", false);
}

static void render_keep_dialog(int x, int y, int w, int h){
    if(!s_keep_ms) return;
    uint32_t now = Timer::GetRealMs();
    uint32_t el = now - s_keep_ms;
    if(el > KEEP_MS) el = KEEP_MS;
    float e = anim_on() ? ease(el, 240) : 1.0f;
    Graphics::FillRectAlpha(x - 16, y, w + 16, h, (uint8_t)(150.0f * e), 0x000000);
    int dw = 400, dh = 178;
    if(dw > w - 16) dw = w - 16;
    int sc = 92 + (int)(8.0f * e);   // scale-in 92% -> 100% (satoru)
    int cw = dw * sc / 100, chh = dh * sc / 100;
    int dx = x - 8 + (w + 8 - cw) / 2, dy = y + (h - chh) / 2;
    shadow(dx, dy, cw, chh, 16, 18, 6, (int)(150.0f * e));
    rr(dx, dy, cw, chh, 16, SettingsUI::T().card, SettingsUI::T().card);
    rr_edge(dx, dy, cw, chh, 16, SettingsUI::T().card_line, 255);
    if(e < 0.98f) return;
    txt(dx + 24, dy + 22, body_px() + 3.0f, "Keep these display settings?", C_FG, true);
    int left = (int)((KEEP_MS - el + 999) / 1000);
    char msg[64] = "Settings will be reverted in ";
    app_int(msg, left, 64);
    SettingsUI::StrApp(msg, left == 1 ? " second." : " seconds.", 64);
    txt(dx + 24, dy + 56, body_px(), msg, SettingsUI::COL_DIM);
    // countdown bar (satoru)
    int pw = cw - 48;
    rr(dx + 24, dy + 88, pw, 6, 3, 0xFF3A3A40, 0xFF3A3A40);
    int fw = (int)((long long)pw * (KEEP_MS - el) / KEEP_MS);
    if(fw > 6) rr(dx + 24, dy + 88, fw, 6, 3, SettingsUI::Accent(), SettingsUI::Accent());
    button(H_KEEP, dx + cw - 24 - 150, dy + chh - 52, 150, 34, "Keep Changes", true);
    button(H_UNDO, dx + cw - 24 - 150 - 10 - 150, dy + chh - 52, 150, 34, "Revert Settings", false);
    Graphics::MarkUIDirty();
}

// cheap signature of the live output state, so a change made elsewhere (the
// `display` shell command, personalize's scale) re-reads the page. (satoru)
static uint32_t live_sig(){
    uint32_t h = 2166136261u ^ (uint32_t)Graphics::GetUIScale() ^ ((uint32_t)Graphics::GetWidth() << 7) ^ (uint32_t)Graphics::GetHeight();
    h = (h ^ IntelModeset::CurrentHz()) * 16777619u;
    for(int i = 0; i < Outputs::Total(); i++){
        const Output* o = Outputs::Get(i);
        if(!o) continue;
        h = (h ^ (uint32_t)(o->connected ? 1 : 0)) * 16777619u;
        h = (h ^ (uint32_t)o->ax) * 16777619u;
        h = (h ^ (uint32_t)o->ay) * 16777619u;
        h = (h ^ (uint32_t)(uintptr_t)o->phys) * 16777619u;
    }
    return h;
}

static void display_render(int x, int y, int w, int h, int scroll){
    s_hot_n = 0;
    s_px = x; s_py = y;
    s_clip_y0 = y; s_clip_y1 = y + h;
    s_last_h = h; s_last_w = w;
    if(s_resync > 0 && --s_resync == 0){
        build_displays();
        Cfg live; read_live(live);
        s_live = live;
        if(!s_keep_ms) s_st = live;
        // after an apply the staged values already match, so no visual jump (satoru)
        s_live_sig = live_sig();
    } else if(s_resync == 0 && !s_keep_ms && s_drag < 0){
        uint32_t sig = live_sig();
        if(s_live_sig && sig != s_live_sig){
            bool untouched = cfg_same(s_st, s_live);
            build_displays();
            read_live(s_live);
            if(untouched) s_st = s_live;
        }
        s_live_sig = sig;
    }
    int cw = w - 16;
    if(cw > 760) cw = 760;
    // a clamp column centred in wide panes, like gnome (satoru)
    if(w - 16 > cw) x += (w - 16 - cw) / 2;
    int ly = y - scroll + 12;
    char buf[64];

    // arrangement canvas (satoru)
    int canvas_h = s_multi ? 250 : 200;
    render_canvas(x, ly, cw, canvas_h);
    ly += canvas_h + 18;

    int mode = mode_of(s_st);
    if(s_multi){
        card_title(x + 4, ly, "DISPLAY MODE");
        ly += 22;
        segmented(x, ly, cw, 38, mode, mirror_possible());
        ly += 38 + 6;
        if(!mirror_possible()){
            txt(x + 6, ly, small_px(), "Mirror needs every display at the same resolution.", SettingsUI::COL_DIM);
        }
        ly += 22;

        // primary display (join + mirror; single uses the selector below) (satoru)
        if(mode != MODE_SINGLE){
            card(x, ly, cw, 52);
            row_label(x, ly, 52, "Primary Display", "Holds the menu bar and the dock");
            int p = s_st.primary;
            SettingsUI::IntToStr(s_d[p].num, buf, 64); SettingsUI::StrApp(buf, "  ", 64);
            SettingsUI::StrApp(buf, s_d[p].name, 64);
            dropdown_row(H_PRIMARY, x, ly, cw, 52, buf);
            ly += 52 + 16;
        }

        card_title(x + 4, ly, mode == MODE_SINGLE ? "USE THIS DISPLAY" : "DISPLAYS");
        ly += 22;
        pills(x, ly, cw, 48);
        ly += 48 + 18;
    }

    // per display settings (satoru)
    {
        int d = s_sel;
        bool show_enable = s_multi && mode != MODE_SINGLE;
        int rows = 3 + (show_enable ? 1 : 0);
        int rh = 50, head = 52;
        int ch = head + rows * rh;
        card(x, ly, cw, ch);
        badge(x + 16, ly + 14, 26, s_d[d].num, SettingsUI::Accent(), 0xFFFFFFFF);
        txt(x + 52, ly + 12, body_px() + 2.0f, s_d[d].name, C_FG, true);
        {
            char sub[64];
            res_str(s_d[d].pw, s_d[d].ph, sub, 64);
            if(s_multi && !s_st.on[d]) SettingsUI::StrCpy(sub, "Off", 64);
            if(d == s_st.primary && s_st.on[d]) SettingsUI::StrApp(sub, "   Primary", 64);
            if(s_d[d].intel) SettingsUI::StrApp(sub, "   Intel pipe B", 64);
            txt(x + 52, ly + 14 + (int)body_px() + 2, small_px(), sub, SettingsUI::COL_DIM);
        }
        int ry = ly + head;
        if(show_enable){
            row_sep(x, ry, cw);
            bool last = s_st.on[d] && on_count(s_st) <= 1;
            bool flash = s_note_ms && Timer::GetRealMs() - s_note_ms < 2500;
            row_label(x, ry, rh, "Enabled", (last && flash) ? "At least one display has to stay on" :
                                           (last ? "This is the only display in use" : nullptr));
            int tx = x + cw - 16 - SettingsUI::TOGGLE_W, ty = ry + (rh - SettingsUI::TOGGLE_H) / 2;
            SettingsUI::Toggle(tx, ty, s_st.on[d]);
            if(last) Graphics::FillRectAlpha(tx, ty, SettingsUI::TOGGLE_W, SettingsUI::TOGGLE_H, 110, C_CARD & 0x00FFFFFF);
            hot(H_ENABLE, tx, ty, SettingsUI::TOGGLE_W, SettingsUI::TOGGLE_H);
            if(flash) Graphics::MarkUIDirty();
            ry += rh;
        }

        // resolution: a selector only where the backend really switches modes (satoru)
        row_sep(x, ry, cw);
        if(!s_multi && switchable_res()){
            bool failed = s_res_fail_ms && Timer::GetRealMs() - s_res_fail_ms < 4000;
            row_label(x, ry, rh, "Resolution", failed ? "That mode could not be set on this display" : nullptr);
            if(failed) Graphics::MarkUIDirty();
            res_str(s_st.rw, s_st.rh, buf, 64);
            dropdown_row(H_RES, x, ry, cw, rh, buf);
        } else {
            const char* why = s_d[d].intel ? "Picked when the external port is lit"
                            : (s_multi ? "Set by the firmware for this output" : "Set by the firmware on this backend");
            row_label(x, ry, rh, "Resolution", why);
            res_str(s_d[d].pw, s_d[d].ph, buf, 64);
            row_value(x, ry, cw, rh, buf);
        }
        ry += rh;

        // monitor refresh: the hardware timing of this output. the external
        // pipe b monitor offers its edid rates and switches live through the
        // apply/keep/revert flow; the internal panel is firmware-owned and
        // read-only (kurono never modesets pipe a). (satoru)
        row_sep(x, ry, cw);
        if(s_d[d].intel){
            int hz = s_st.hz > 0 ? s_st.hz : (int)IntelModeset::CurrentHz();
            SettingsUI::IntToStr(hz, buf, 64); SettingsUI::StrApp(buf, " Hz", 64);
            row_label(x, ry, rh, "Monitor Refresh", "Hardware timing on the external port");
            dropdown_row(H_DPRATE, x, ry, cw, rh, buf);
        } else {
            bool panel = d == s_st.primary || !s_multi;
            uint32_t mhz = panel ? FramePacer::PanelHz() : 0;
            if(mhz){ SettingsUI::IntToStr((int)mhz, buf, 64); SettingsUI::StrApp(buf, " Hz", 64); }
            else SettingsUI::StrCpy(buf, "Firmware mode", 64);
            row_label(x, ry, rh, "Monitor Refresh", panel ? "Panel runs the firmware mode" : "Firmware mode");
            row_value(x, ry, cw, rh, buf);
        }
        ry += rh;

        // scale: the existing ui scale, one value for every display (satoru)
        row_sep(x, ry, cw);
        row_label(x, ry, rh, "Scale", s_multi ? "Applies to every display" : nullptr);
        SettingsUI::IntToStr(s_st.scale, buf, 64); SettingsUI::StrApp(buf, " %", 64);
        dropdown_row(H_SCALE, x, ry, cw, rh, buf);
        ry += rh;
        ly += ch + 22;
    }

    // compositor frame pacing (not the monitor's hardware timing), live (satoru)
    {
        card_title(x + 4, ly, "FRAME RATE");
        ly += 22;
        int rh = 50;
        card(x, ly, cw, rh * 2);
        if(FramePacer::CmdlineHz()){
            SettingsUI::IntToStr((int)FramePacer::CmdlineHz(), buf, 64); SettingsUI::StrApp(buf, " Hz (kurono.hz)", 64);
        } else if(s_refresh_mode == 0){
            SettingsUI::StrCpy(buf, "Auto (", 64); app_int(buf, (int)FramePacer::CapHz(), 64); SettingsUI::StrApp(buf, " Hz)", 64);
        } else {
            SettingsUI::IntToStr(s_refresh_mode, buf, 64); SettingsUI::StrApp(buf, " Hz", 64);
        }
        char sub[64] = "Compositor ";
        app_int(sub, (int)FramePacer::CurrentHz(), 64); SettingsUI::StrApp(sub, " Hz", 64);
        if(FramePacer::CurrentHz() < FramePacer::CapHz()) SettingsUI::StrApp(sub, " (stepped down)", 64);
        else if(s_refresh_mode > (int)FramePacer::CapHz()) SettingsUI::StrApp(sub, " (output capped)", 64);
        row_label(x, ly, rh, "Frame Rate Limit", sub);
        dropdown_row(H_RATE, x, ly, cw, rh, buf);
        row_sep(x, ly + rh, cw);
        row_label(x, ly + rh, rh, "VSync", nullptr);
        int tx = x + cw - 16 - SettingsUI::TOGGLE_W, ty = ly + rh + (rh - SettingsUI::TOGGLE_H) / 2;
        SettingsUI::Toggle(tx, ty, s_vsync);
        hot(H_VSYNC, tx, ty, SettingsUI::TOGGLE_W, SettingsUI::TOGGLE_H);
        ly += rh * 2 + 22;
    }

    // brightness, live like before (satoru)
    {
        card_title(x + 4, ly, "BRIGHTNESS");
        ly += 22;
        int rh = 56;
        card(x, ly, cw, rh);
        row_label(x, ly, rh, "Brightness", "Saved as a preference, no hardware dimming here");
        int sl_w = cw / 3;
        int sx = x + cw - 16 - 44 - sl_w, sy = ly + rh / 2 - 8;
        SettingsUI::Slider(sx, sy, sl_w, s_brightness);
        hot(H_BRIGHT, sx, sy - 4, sl_w, SettingsUI::SLIDER_H + 12);
        SettingsUI::IntToStr(s_brightness, buf, 64); SettingsUI::StrApp(buf, "%", 64);
        txt(sx + sl_w + 12, ly + (rh - (int)body_px()) / 2 - 1, body_px(), buf, SettingsUI::COL_DIM);
        ly += rh + 22;
    }

    // night light: on/off, the daily schedule and the warmth, all live (satoru)
    {
        card_title(x + 4, ly, "NIGHT LIGHT");
        ly += 22;
        int rh = 50;
        card(x, ly, cw, rh * 3);
        int tx = x + cw - 16 - SettingsUI::TOGGLE_W;
        row_label(x, ly, rh, "Night Light", "A warmer screen that is easier on the eyes at night");
        int ty = ly + (rh - SettingsUI::TOGGLE_H) / 2;
        SettingsUI::Toggle(tx, ty, NightLight::IsOn());
        hot(H_NIGHT, tx, ty, SettingsUI::TOGGLE_W, SettingsUI::TOGGLE_H);
        row_sep(x, ly + rh, cw);
        char sched[64];
        int fm = NightLight::FromMinutes(), tm = NightLight::ToMinutes();
        char a[8] = "00:00", b[8] = "00:00";
        a[0] = (char)('0' + fm / 600); a[1] = (char)('0' + (fm / 60) % 10); a[3] = (char)('0' + (fm % 60) / 10); a[4] = (char)('0' + fm % 10);
        b[0] = (char)('0' + tm / 600); b[1] = (char)('0' + (tm / 60) % 10); b[3] = (char)('0' + (tm % 60) / 10); b[4] = (char)('0' + tm % 10);
        SettingsUI::StrCpy(sched, "Turns on at ", 64); SettingsUI::StrApp(sched, a, 64);
        SettingsUI::StrApp(sched, ", off at ", 64); SettingsUI::StrApp(sched, b, 64);
        row_label(x, ly + rh, rh, "Schedule", sched);
        ty = ly + rh + (rh - SettingsUI::TOGGLE_H) / 2;
        SettingsUI::Toggle(tx, ty, NightLight::ScheduleOn());
        hot(H_NSCHED, tx, ty, SettingsUI::TOGGLE_W, SettingsUI::TOGGLE_H);
        row_sep(x, ly + rh * 2, cw);
        row_label(x, ly + rh * 2, rh, "Warmth", nullptr);
        int sl_w = cw / 3;
        int sx = x + cw - 16 - 44 - sl_w, sy = ly + rh * 2 + rh / 2 - 8;
        SettingsUI::Slider(sx, sy, sl_w, NightLight::Strength());
        hot(H_NSTR, sx, sy - 4, sl_w, SettingsUI::SLIDER_H + 12);
        SettingsUI::IntToStr(NightLight::Strength(), buf, 64); SettingsUI::StrApp(buf, "%", 64);
        txt(sx + sl_w + 12, ly + rh * 2 + (rh - (int)body_px()) / 2 - 1, body_px(), buf, SettingsUI::COL_DIM);
        ly += rh * 3;
    }
    ly += 84;     // room for the action bar (satoru)
    s_content_h = ly - (y - scroll);

    render_action_bar(x, y, cw, h);
    render_keep_dialog(s_px, y, s_last_w, h);
}

// ── input (satoru) ──────────────────────────────────────────────────────────
static void step_dropdown(int id, int dir){
    if(id == H_PRIMARY){
        // cycle through the displays that are on (satoru)
        int p = s_st.primary;
        for(int k = 1; k <= s_n; k++){
            int c = (p + (dir > 0 ? k : s_n - k)) % s_n;
            if(s_st.on[c]){ set_primary(c); s_sel = c; break; }
        }
    } else if(id == H_RES){
        // walk the supported modes by area from the staged size, which may
        // itself not be in the list (a firmware-chosen mode). (satoru)
        long long cur = (long long)s_st.rw * s_st.rh;
        int best = -1; long long ba = 0;
        for(int i = 0; i < s_mode_count; i++){
            if(DisplayManager::GetBackend() == DISPLAY_BACKEND_BGA && (s_modes[i].width & 7)) continue;
            long long a = (long long)s_modes[i].width * s_modes[i].height;
            if((dir > 0 && a > cur && (best < 0 || a < ba)) || (dir < 0 && a < cur && (best < 0 || a > ba))){ best = i; ba = a; }
        }
        if(best >= 0){
            s_st.res = best;
            s_st.rw = (int)s_modes[best].width; s_st.rh = (int)s_modes[best].height;
            s_d[0].pw = s_st.rw; s_d[0].ph = s_st.rh;
        }
    } else if(id == H_SCALE){
        int idx = 0;
        for(int i = 0; i < kScaleCount; i++) if(kScales[i] <= s_st.scale) idx = i;
        idx += dir;
        if(idx < 0) idx = 0;
        if(idx >= kScaleCount) idx = kScaleCount - 1;
        s_st.scale = kScales[idx];
    } else if(id == H_RATE){
        int cur = 0;
        for(int i = 0; i < kRateCount; i++) if(kRates[i] == s_refresh_mode) cur = i;
        int nxt = cur + dir;
        if(nxt < 0) nxt = 0;
        if(nxt >= kRateCount) nxt = kRateCount - 1;
        if(nxt != cur){ s_refresh_mode = kRates[nxt]; FramePacer::SetMode((uint32_t)s_refresh_mode); }
    } else if(id == H_DPRATE){
        // the monitor's own timings, not the pacer ladder. (satoru)
        uint32_t rates[8];
        int n = IntelModeset::Rates(rates, 8);
        if(n <= 0) return;
        int cur = s_st.hz > 0 ? s_st.hz : (int)IntelModeset::CurrentHz();
        int idx = 0;
        for(int i = 0; i < n; i++) if((int)rates[i] <= cur) idx = i;
        idx += dir;
        if(idx < 0) idx = 0;
        if(idx >= n) idx = n - 1;
        s_st.hz = (int)rates[idx];
    }
}

static bool display_input(int mx, int my, bool click, char key, int scroll){
    (void)scroll;
    if(!click){
        if(s_keep_ms){
            if(key == 27){ do_undo(); return true; }
            if(key == '\n' || key == '\r'){ do_keep(); return true; }
        }
        return false;
    }
    if(s_keep_ms){
        const Hot* h = hot_at(mx, my, false);
        if(h && h->id == H_KEEP){ do_keep(); return true; }
        if(h && h->id == H_UNDO){ do_undo(); return true; }
        return true;    // modal: swallow everything else (satoru)
    }
    const Hot* h = hot_at(mx, my, false);
    if(!h) return false;
    int id = h->id;
    if(id >= H_TILE0 && id <= H_TILE3){
        int d = id - H_TILE0;
        if(d < s_n){
            s_sel = d;
            if(mode_of(s_st) != MODE_MIRROR){
                // start a drag; frozen fit so the canvas does not move under the cursor (satoru)
                s_drag = d; s_drag_moved = false;
                s_press_x = mx + s_px; s_press_y = my + s_py;
                s_grab_x = s_press_x - (h->x + s_px); s_grab_y = s_press_y - (h->y + s_py);
                s_drag_fx = s_st.ax[d]; s_drag_fy = s_st.ay[d];
                // recover the fit the canvas used this frame from the tile rect (satoru)
                s_fcs = s_d[d].pw ? (float)h->w / (float)s_d[d].pw : 0.1f;
                s_fox = h->x + s_px - (int)((float)s_draw_x[d] * s_fcs);
                s_foy = h->y + s_py - (int)((float)s_draw_y[d] * s_fcs);
                s_frozen = true;
            }
        }
        return true;
    }
    switch(id){
        case H_IDENT: s_ident_ms = Timer::GetRealMs(); if(!s_ident_ms) s_ident_ms = 1; return true;
        case H_MODE0: case H_MODE1: case H_MODE2: set_mode(id - H_MODE0); return true;
        case H_PILL0: case H_PILL1: case H_PILL2: case H_PILL3: {
            int d = id - H_PILL0;
            s_sel = d;
            if(mode_of(s_st) == MODE_SINGLE && d != s_st.primary){
                begin_anim();
                s_st.on[s_st.primary] = false;
                s_st.on[d] = true; s_st.primary = d;
                s_st.ax[d] = 0; s_st.ay[d] = 0;
            }
            return true;
        }
        case H_ENABLE: set_enabled(s_sel, !s_st.on[s_sel]); return true;
        case H_PRIMARY: case H_RES: case H_SCALE: case H_RATE: case H_DPRATE: {
            int dir = SettingsUI::DropdownHit(h->x, h->y, h->w, mx, my);
            if(dir < 0){
                // a click on the value pill steps forward (satoru)
                dir = 1;
            } else dir = dir == 0 ? -1 : 1;
            step_dropdown(id, dir);
            return true;
        }
        case H_VSYNC:
            s_vsync = !s_vsync; persist_vsync(); FramePacer::ReloadConfig(); return true;
        case H_BRIGHT: {
            int p = SettingsUI::SliderHit(h->x, h->y + 4, h->w, mx, my);
            if(p >= 0){ s_brightness = p; persist_brightness(); }
            return true;
        }
        case H_NIGHT: NightLight::Toggle(); return true;
        case H_NSCHED: NightLight::SetSchedule(!NightLight::ScheduleOn()); return true;
        case H_NSTR: {
            int p = SettingsUI::SliderHit(h->x, h->y + 4, h->w, mx, my);
            if(p >= 0) NightLight::SetStrength(p);
            return true;
        }
        case H_APPLY: do_apply(); return true;
        case H_REVERT:
            begin_anim(); s_st = s_live;
            if(!s_multi && s_st.rw > 0){ s_d[0].pw = s_st.rw; s_d[0].ph = s_st.rh; }
            return true;
    }
    return false;
}

static int display_content_height(){ return s_content_h; }

// ── hooks (settings_display.h) (satoru) ───────────────────────────────────
namespace DisplaySettings {

static bool parse_entry(const char* v, char* name, int* pw, int* ph, int* on, int* ax, int* ay){
    int i = 0, n = 0;
    while(v[i] && v[i] != ',' && n < 15) name[n++] = v[i++];
    name[n] = 0;
    if(v[i] != ',') return false;
    int* f[5] = {pw, ph, on, ax, ay};
    for(int k = 0; k < 5; k++){
        if(v[i] != ',') return false;
        i++;
        bool neg = false;
        if(v[i] == '-'){ neg = true; i++; }
        if(v[i] < '0' || v[i] > '9') return false;
        int x = 0;
        while(v[i] >= '0' && v[i] <= '9') x = x * 10 + (v[i++] - '0');
        *f[k] = neg ? -x : x;
    }
    return true;
}

void RestoreAtBoot(){
    if(!Graphics::MultiActive()) return;
    int total = Outputs::Total();
    if(UIConfig::Int("display.outputs", 0) != total) return;
    bool on[OUTPUT_MAX] = {false, false, false, false};
    int ax[OUTPUT_MAX] = {0, 0, 0, 0}, ay[OUTPUT_MAX] = {0, 0, 0, 0};
    bool used[OUTPUT_MAX] = {false, false, false, false};
    int prim = -1;
    for(int k = 0; k < total; k++){
        char key[20] = "display.out";
        app_int(key, k, 20);
        const char* v = UIConfig::Str(key, nullptr);
        if(!v) return;
        char nm[16]; int pw, ph, o, x, y;
        if(!parse_entry(v, nm, &pw, &ph, &o, &x, &y)) return;
        int slot = -1;
        for(int i = 0; i < total; i++){
            const Output* out = Outputs::Get(i);
            if(!used[i] && out && out->pw == pw && out->ph == ph && s_eq(out->name, nm)){ slot = i; break; }
        }
        if(slot < 0){
            SerialLogger::Log("[displays] saved arrangement is for other outputs, not restored\r\n");
            return;
        }
        used[slot] = true;
        on[slot] = o != 0; ax[slot] = x; ay[slot] = y;
        if(k == 0) prim = slot;
    }
    if(prim < 0 || !on[prim] || ax[prim] != 0 || ay[prim] != 0) return;
    // sanity: on-screen, and no overlaps unless it is a mirror (satoru)
    bool mirror = true;
    for(int i = 0; i < total; i++) if(on[i] && (ax[i] || ay[i])) mirror = false;
    for(int i = 0; i < total; i++){
        if(!on[i]) continue;
        if(ax[i] < 0 || ay[i] < 0) return;
        const Output* a = Outputs::Get(i);
        for(int j = i + 1; j < total && !mirror; j++){
            if(!on[j]) continue;
            const Output* b = Outputs::Get(j);
            if(ov(ax[i], ay[i], a->pw, a->ph, ax[j], ay[j], b->pw, b->ph)) return;
        }
        if(mirror && (a->pw != Outputs::Get(prim)->pw || a->ph != Outputs::Get(prim)->ph)) return;
    }
    Graphics::RequestOutputConfig(prim, on, ax, ay);
    Graphics::ApplyPendingOutputOp();
    int ox[OUTPUT_MAX], oy[OUTPUT_MAX], sa, sb;
    Graphics::TakeOutputReloc(ox, oy, &sa, &sb);   // no windows yet: drop it (satoru)
    SerialLogger::Log("[displays] restored the saved arrangement\r\n");
}

void Overlay(){
    uint32_t now = Timer::GetRealMs();
    // keep-dialog timeout runs here so it fires even when settings is not redrawn (satoru)
    if(s_reopen){
        if(!SystemSettings::IsOpen()){ s_reopen = false; SystemSettings::Open(); }
        else if(now - s_reopen_ms > 3000) s_reopen = false;
    }
    if(s_keep_ms){
        if(!SystemSettings::IsOpen() || now - s_keep_ms >= KEEP_MS) do_undo();
        else Graphics::MarkUIDirty();
    }
    if(!s_ident_ms) return;
    const uint32_t DUR = 2600;
    uint32_t el = now - s_ident_ms;
    int total = Outputs::Multi() ? Outputs::Total() : 0;
    if(el >= DUR || total < 2){
        s_ident_ms = 0;
        for(int i = 0; i < total; i++){
            const Output* o = Outputs::Get(i);
            if(o && o->connected) Graphics::MarkDirty(o->x, o->y, o->w, o->h);
        }
        Graphics::MarkUIDirty();
        return;
    }
    int a = 235;
    if(el < 160) a = (int)(235 * el / 160);
    else if(el > DUR - 320) a = (int)(235 * (DUR - el) / 320);
    int stack = 0;
    for(int i = 0; i < total; i++){
        const Output* o = Outputs::Get(i);
        if(!o || !o->connected) continue;
        // mirrored outputs share a rect: put their numbers side by side (satoru)
        int dup = 0;
        for(int j = 0; j < i; j++){
            const Output* p = Outputs::Get(j);
            if(p && p->connected && p->x == o->x && p->y == o->y) dup++;
        }
        (void)stack;
        int sz = o->h / 4; if(sz < 120) sz = 120; if(sz > 260) sz = 260;
        int bx = o->x + 48 + dup * (sz + 24), by = o->y + 48 + (i == 0 ? 28 : 0);
        MDraw::Shadow(bx, by, sz, sz, sz / 5, 18, 6, a / 2);
        uint32_t acc = SettingsUI::Accent();
        MDraw::RoundRect(bx, by, sz, sz, sz / 5, MDraw::Mix(acc, 0xFFFFFFFF, 40), acc, a);
        char b[4]; SettingsUI::IntToStr(parse_num(o->name, i + 1), b, 4);
        float px = (float)sz * 0.62f;
        int tw = FontTTF::ok ? FontTTF::MeasureText(px, b, true) : 8 * (int)(px / 16.0f);
        uint32_t fg = 0x00FFFFFFu | ((uint32_t)a << 24);
        if(FontTTF::ok) FontTTF::DrawText(bx + (sz - tw) / 2, by + (sz - (int)px) / 2 - (int)(px * 0.06f), px, b, fg, true);
        else FontTTF::DrawPixel(bx + (sz - tw) / 2, by + (sz - (int)px) / 2, px, b, fg);
        Graphics::MarkDirty(bx - 24, by - 24, sz + 48, sz + 56);
    }
    Graphics::MarkUIDirty();
}

}   // namespace DisplaySettings (satoru)

// `extern` forces EXTERNAL linkage on this const definition so the shell's
// `extern const SettingsModule g_display_module;` resolves at link time. (satoru)
extern const SettingsModule g_display_module = {
    "display", "Displays", "\x0F",
    display_on_show, display_render, display_input, display_content_height, nullptr
};
// end (satoru)
