#include "window_manager.h"
#include "../drivers/graphics.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include "../drivers/keyboard.h"
#include "../drivers/display_mgr.h"
#include "../drivers/outputs.h"   // multi-monitor outputs (satoru)
#include "../drivers/mouse.h"   // cursor pos for fast-path damage (satoru)
#include "../system/ui_config.h"
#include "../kernel/pmm.h"   // drag-backdrop buffer alloc/free (satoru)
#include "../kernel/heap.h"  // alt-tab thumbnail buffers (satoru)
#include "../fs/kvfs.h"      // per-app window geometry memory (satoru)
#include "kss.h"             // motion tokens + live (scriptable) accent (satoru)
#include "ui_elements.h"     // Animation::LerpColor for the traffic-light ease (satoru)
#include "wm_motion.h"       // spring/bezier curves + snapshot blitter (satoru)
#include "desktop.h"         // Taskbar tile rect/colour for the motion origin (satoru)
#include "style.h"           // modern vs classic chrome (satoru)
#include "modern_draw.h"     // anti-aliased modern window chrome (satoru)
#include "font.h"
#include "app_icons.h"       // per-window app logo in the titlebar (satoru)
#include "wayland_server.h"   // keyboard focus follows the wm focus (satoru)
#include "frame_pacer.h"     // refresh cap for pacing + the drag throttle (satoru)
#include "../drivers/cpu_detect.h"   // tsc rate for the motion frame-cost probe (satoru)
#include "../kernel/taskmem.h"   // kurono.taskmem=1 app scopes (satoru)
#include "ui_diag.h"   // kurono.uidiag=1 pointer/drag counters (satoru)
#include "control_center.h"   // move by copy: overlays that draw over windows (satoru)
#include "notification.h"

#if defined(__GNUC__) || defined(__clang__)
#  define WM_LIKELY(x)   __builtin_expect(!!(x), 1)
#  define WM_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#  define WM_LIKELY(x)   (x)
#  define WM_UNLIKELY(x) (x)
#endif

//  window manager implementation

// ───────────────────────────────────────────────────────────────
// Compositor configuration cache.  Populated from UIConfig at boot
// and re-read by ReloadFromConfig().  Kept as plain statics so the
// per-frame render path stays branch-light.
// ───────────────────────────────────────────────────────────────
static bool comp_shadow_enabled       = true;
static int  comp_shadow_radius        = 8;        // 0..16
static int  comp_shadow_opacity_pct   = 60;       // 0..100
static bool comp_shadow_during_drag   = false;
static bool comp_animations_enabled   = true;
static int  comp_anim_duration_ms     = 180;   // open/close fade+scale, perceptible but snappy (satoru)
static int  comp_default_alpha        = 255;
static bool comp_frosted_titlebar     = true;
static bool comp_reduced_motion       = false;

// window motion lengths in design-ms (see "window motion" below). (satoru)
static const int WM_OPEN_MS  = 660;   // 40 ms tile dip + 620 ms spring settle
static const int WM_CLOSE_MS = 340;   // inhale, fold to titlebar, pill sinks
static const int WM_MIN_MS   = 560;   // 198 ms row stagger + 360 ms pour
static const int WM_UNMIN_MS = 900;   // 80 ms press + 198 ms stagger + 620 ms spring
static unsigned short wm_motion_dur(int design_ms);
static void wm_snap_release(Window* win);
// spring motion (compositor.motion = spring | classic), see "spring motion"
// below. helpers are defined with the motion renderers. (satoru)
static bool wm_spring = true;
enum { FX_NONE = 0, FX_OPEN = 1, FX_CLOSE = 2, FX_MIN = 3, FX_UNMIN = 4, FX_MORPH = 5 };
static bool wm_spring_on();
static void wm_fx_init(Window* w);
static void wm_fx_old_release(Window* w);
static void wm_fx_morph(Window* w, int ox, int oy, int ow, int oh, bool old_tb);
static int  wm_next_focus(const Window* wins, int leaving);
static unsigned long long wm_now_us();
static bool wm_anim_on();
static int  wm_radius(const Window* w);
static void wm_damage_corners(const Window* win);   // focus ease corner damage (satoru)
static void wm_fx_damage(Window* w, int x0, int y0, int x1, int y1);
static void wm_tab_thumb_grab(Window* win, bool force = false);
static void wm_tab_thumb_free_all();
static void wm_snap_preview_clear();

// Drag throttle. Updated by ReloadFromConfig() from display.refresh_hz.
// 0 means "no throttle"; otherwise per-frame minimum gap in ms.
static unsigned int wm_drag_min_gap_ms = 6;       // ~165 Hz upper bound
static unsigned int wm_last_drag_ms    = 0;

// ───────────────────────────────────────────────────────────────
// Cached-desktop snapshot used during a titlebar drag. The backdrop
// holds the whole composited desktop MINUS the dragged window so each
// drag frame is one backdrop blit + the single moving window instead
// of a full re-composite. Allocated once (lazily) at screen size and
// reused for every drag; freed only if the screen geometry changes.
// (satoru)
// ───────────────────────────────────────────────────────────────
static uint8_t*     wm_backdrop          = nullptr; // offscreen rgba copy
static size_t       wm_backdrop_bytes    = 0;       // for PMM::FreeBytes
static int          wm_backdrop_w        = 0;       // pixels it was sized for
static int          wm_backdrop_h        = 0;
static uint32_t     wm_backdrop_pitch    = 0;       // byte stride captured with
static bool         wm_backdrop_ready    = false;   // holds a valid snapshot
static bool         wm_backdrop_capturing= false;   // omit dragged win this frame
static int          wm_backdrop_win_id   = -1;      // window the snapshot excludes
static unsigned int wm_backdrop_built_ms = 0;       // age cap source (satoru)
// set only while the fast drag path is repainting the moving window, so the
// damage choke point below does NOT mistake the window's own movement for a
// structural change and drop the backdrop. (satoru)
static bool         wm_in_fast_render    = false;
// previous-frame footprint of the dragged window so the fast path can
// repaint where it used to be from the backdrop. (satoru)
static int          wm_drag_prev_x = 0, wm_drag_prev_y = 0;
static int          wm_drag_prev_w = 0, wm_drag_prev_h = 0;
static bool         wm_drag_prev_valid = false;
// previous-frame cursor hotspot: the OS draws the cursor after Render, so when
// the pointer detaches from the window (e.g. dragged into a clamp edge) the
// fast path must still restore the backdrop where the cursor was and damage
// where it is, or it smears. (satoru)
static int          wm_drag_cur_px = 0, wm_drag_cur_py = 0;
static bool         wm_drag_cur_valid = false;
// a backdrop older than this is rebuilt so a long drag still picks up the
// once-a-second clock tick / any late toast behind the window. (satoru)
#define WM_BACKDROP_MAX_AGE_MS 800u

// ───────────────────────────────────────────────────────────────
// Window context menu state (right-click on a titlebar). Kept as
// plain statics - there was no reusable wm-level menu facility to
// extend, so this is a minimal self-contained popup. (satoru)
// ───────────────────────────────────────────────────────────────
static bool wm_ctx_open   = false;
static int  wm_ctx_win_id = -1;       // window the menu acts on
static int  wm_ctx_x      = 0;        // top-left of the popup
static int  wm_ctx_y      = 0;
static unsigned int wm_ctx_open_ms = 0;   // drives the fade+unfold on open (satoru)
#define WM_CTX_W        210
#define WM_CTX_ITEM_H    28
// entries: 0 move to other monitor, 1 full screen, 2 maximize/restore,
// 3 minimize, 4 close (satoru)
#define WM_CTX_ITEMS      5

Window WindowManager::windows[WM_MAX_WINDOWS];
int WindowManager::window_count = 0;
int WindowManager::next_id = 1;
int WindowManager::focused_id = -1;
int WindowManager::screen_width = 1024;
int WindowManager::screen_height = 768;
static int wm_desktop_x = 0;
static int wm_desktop_y = 0;
static int wm_desktop_w = 1024;
static int wm_desktop_h = 728;
WMAction WindowManager::current_action = WM_NONE;
int WindowManager::action_window_id = -1;
int WindowManager::drag_offset_x = 0;
int WindowManager::drag_offset_y = 0;
bool WindowManager::prev_mouse_down = false;  
bool WindowManager::mouse_is_down = false;
static int wm_input_capture_id = -1;

static int wmlen(const char* s) {
    if (!s) return 0;
    int n=0; while(s[n]) n++; return n;
}
static void wmcpy(char* d, const char* s, int m) {
    if (!d || m <= 0) return;
    if (!s) { d[0] = 0; return; }
    int i=0; while(s[i]&&i<m-1) { d[i]=s[i]; i++; } d[i]=0;
}

// Damage helper - push a rect to Graphics' dirty-region tracker, but
// only the portion that lies on-screen.  Cheap to call.
static inline void wm_damage(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    // a window sized rect carries its shadow: an elevated shadow reaches 16 px further down
    // than the margins the callers use (offset 8..16, blur 22..38), and under the damage
    // scissor the faint tail stayed behind (1 level, 2 rows). (satoru)
    if (h >= 2 * WM_SHADOW_SIZE && Graphics::ScissorEnabled()) h += 16;
    int sw = WindowManager::GetScreenWidth();
    int sh = WindowManager::GetScreenHeight();
    if (x >= sw || y >= sh) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > sw) w = sw - x;
    if (y + h > sh) h = sh - y;
    if (w <= 0 || h <= 0) return;
    Graphics::MarkDirty(x, y, w, h);
    // Any WM-side damage (create/close/move/resize/focus/snap/minimize/
    // maximize/restore/cross-monitor) implies the composite changed, so
    // also raise the global frame signal. Single choke point keeps every
    // WindowManager mutator covered without per-call edits. (satoru)
    Graphics::MarkUIDirty();
}

// footprint damage of the window being dragged, remembered until the next
// frame renders, so a move by copy (kurono.gfx.scissor=1) can take exactly
// these rects back and repaint only what the move uncovered. (satoru)
static int wm_last_top_id = -1;      // top window of the last rendered frame (satoru)
static Graphics::Rect wm_fp_rects[16];
static int wm_fp_n = 0;
static bool wm_fp_overflow = false;
static inline void wm_fp_damage(int x, int y, int w, int h) {
    wm_damage(x, y, w, h);
    if (wm_fp_n < 16) wm_fp_rects[wm_fp_n++] = Graphics::Rect{ x, y, w, h };
    else wm_fp_overflow = true;
}

// Damage a window's last and current rect, with shadow margin.
static inline void wm_damage_window(const Window* w) {
    if (!w) return;
    const int m = WM_SHADOW_SIZE + 2;
    // the elevated shadow reaches 16 px further down than the margin (offset 8..16 plus blur 22..38,
    // under the damage scissor a faint tail stayed behind a closed window): (satoru)
    const int xb = Graphics::ScissorEnabled() ? 16 : 0;
    if (w->had_last) {
        wm_damage(w->last_x - m, w->last_y - m,
                  w->last_w + 2*m, w->last_h + 2*m + xb);
    }
    if (w->state != WIN_CLOSED && w->visible && w->state != WIN_MINIMIZED) {
        wm_damage(w->x - m, w->y - m, w->w + 2*m, w->h + 2*m + xb);
    }
    // every window lifecycle/structural change (create/close/min/max/restore/
    // snap/focus/move/resize/visibility/cross-monitor) funnels through here,
    // and the per-frame drag movement does NOT (it damages explicit rects via
    // wm_damage). so this is the right choke point to drop a stale drag
    // backdrop: the set/geometry of windows BEHIND the dragged one changed.
    // the guard exempts the fast path's own repaint of the moving window.
    // (satoru)
    if (!wm_in_fast_render) WindowManager::InvalidateDragBackdrop();
}
// multi-monitor work area of output i: output 0 keeps the menu bar / dock
// reservation (wm_desktop_*), every other output is used whole. with a single
// output this is exactly wm_desktop_*, so one-screen behaviour is unchanged. (satoru)
static void wm_area_of(int i, int* ax, int* ay, int* aw, int* ah) {
    const Output* o = (Outputs::Multi() && i > 0) ? Outputs::Get(i) : nullptr;
    if (o && o->connected) {
        *ax = o->x; *ay = o->y; *aw = o->w; *ah = o->h;
        return;
    }
    *ax = wm_desktop_x; *ay = wm_desktop_y; *aw = wm_desktop_w; *ah = wm_desktop_h;
}
// work area of the output holding point (px,py), nearest one if it is in a gap.
static void wm_area_at(int px, int py, int* ax, int* ay, int* aw, int* ah) {
    int i = 0;
    if (Outputs::Multi()) {
        i = Outputs::At(px, py);
        if (i < 0) i = Outputs::Nearest(px, py);
    }
    wm_area_of(i, ax, ay, aw, ah);
}
// work area for something new: the output under the cursor.
static void wm_area_cursor(int* ax, int* ay, int* aw, int* ah) {
    wm_area_at(Mouse::mx, Mouse::my, ax, ay, aw, ah);
}

static void wm_clamp_drag_bounds(Window* win) {
    if (!win) return;
    if (Outputs::Multi()) {
        // the window may straddle a seam while dragged: clamp to the whole virtual
        // desktop, then make sure its titlebar centre sits on a live output (not
        // in the dead zone beside a shorter screen) and below that output's menu
        // bar. (satoru)
        int ww = win->w > 0 ? win->w : 1, wh = win->h > 0 ? win->h : 1;
        int vw = WindowManager::GetScreenWidth(), vh = WindowManager::GetScreenHeight();
        int max_x = vw - ww, max_y = vh - wh;
        if (max_x < 0) max_x = 0;
        if (max_y < 0) max_y = 0;
        if (win->x < 0) win->x = 0;
        if (win->x > max_x) win->x = max_x;
        if (win->y < 0) win->y = 0;
        if (win->y > max_y) win->y = max_y;
        int tx = win->x + ww / 2, ty = win->y + 12;
        int px = tx, py = ty;
        Outputs::Constrain(&px, &py);
        win->x += px - tx;
        win->y += py - ty;
        int ax, ay, aw, ah;
        wm_area_at(win->x + ww / 2, win->y + 12, &ax, &ay, &aw, &ah);
        if (win->y < ay) win->y = ay;
        int low = ay + ah - (wh < 48 ? wh : 48);
        if (win->y > low) win->y = low;
        return;
    }
    int visible_w = win->w < 96 ? win->w : 96;
    int visible_h = win->has_titlebar ? WM_TITLEBAR_H : 24;
    if (visible_h < 16) visible_h = 16;
    int win_w = win->w > 0 ? win->w : 1;
    int win_h = win->h > 0 ? win->h : 1;
    // keep the whole window on-screen so it can't be dragged off an edge and
    // get lost / appear to "wrap" to the other side. oversized windows pin to
    // the top-left via the max<min guards below. (satoru)
    (void)visible_w; (void)visible_h;
    int min_x = wm_desktop_x;
    int max_x = wm_desktop_x + wm_desktop_w - win_w;
    int min_y = wm_desktop_y;
    int max_y = wm_desktop_y + wm_desktop_h - win_h;
    if (max_x < min_x) max_x = min_x;
    if (max_y < min_y) max_y = min_y;
    if (win->x < min_x) win->x = min_x;
    if (win->x > max_x) win->x = max_x;
    if (win->y < min_y) win->y = min_y;
    if (win->y > max_y) win->y = max_y;
    (void)win_h;
}
static void wm_clamp_resize_bounds(Window* win) {
    if (!win) return;
    int rax, ray, raw_, rah;
    wm_area_at(win->x + win->w / 2, win->y + 12, &rax, &ray, &raw_, &rah);
    int max_w = rax + raw_ - win->x;
    int max_h = ray + rah - win->y;
    if (max_w < WM_MIN_WIDTH) max_w = WM_MIN_WIDTH;
    if (max_h < WM_MIN_HEIGHT) max_h = WM_MIN_HEIGHT;
    if (win->w < WM_MIN_WIDTH) win->w = WM_MIN_WIDTH;
    if (win->h < WM_MIN_HEIGHT) win->h = WM_MIN_HEIGHT;
    if (win->w > max_w) win->w = max_w;
    if (win->h > max_h) win->h = max_h;
}

// work area of the output a window lives on (its monitor id, not the pointer
// position), so maximize and snap follow the window across outputs. (satoru)
static void wm_window_area(Window* win, int* ax, int* ay, int* aw, int* ah) {
    int m = win ? win->monitor_id : 0;
    if (!Outputs::Multi() || m < 0 || m >= OUTPUT_MAX || !Outputs::IsLive(m)) m = 0;
    wm_area_of(m, ax, ay, aw, ah);
}

// full rect of output `i` (menu bar and dock included). falls back to the whole
// virtual desktop when there is no output table. (satoru)
static void wm_output_rect(int i, int* ox, int* oy, int* ow, int* oh) {
    if (Outputs::Multi()) {
        const Output* o = Outputs::Get(i);
        if (o && o->connected && o->w > 0 && o->h > 0) {
            *ox = o->x; *oy = o->y; *ow = o->w; *oh = o->h;
            return;
        }
    }
    *ox = 0; *oy = 0;
    *ow = WindowManager::GetScreenWidth();
    *oh = WindowManager::GetScreenHeight();
}

// target rect of a snap edge inside a work area. (satoru)
static void wm_snap_rect(int edge, int ax, int ay, int aw, int ah,
                         int* nx, int* ny, int* nw, int* nh) {
    int halfw = aw / 2;
    int halfh = ah / 2;
    *nx = ax; *ny = ay; *nw = aw; *nh = ah;
    switch (edge) {
        case 0: *nx = ax;         *ny = ay;          *nw = halfw;      *nh = ah;          break;
        case 1: *nx = ax + halfw; *ny = ay;          *nw = aw - halfw; *nh = ah;          break;
        case 2: break;
        case 4: *nx = ax;         *ny = ay;          *nw = halfw;      *nh = halfh;       break;
        case 5: *nx = ax + halfw; *ny = ay;          *nw = aw - halfw; *nh = halfh;       break;
        case 6: *nx = ax;         *ny = ay + halfh;  *nw = halfw;      *nh = ah - halfh;  break;
        case 7: *nx = ax + halfw; *ny = ay + halfh;  *nw = aw - halfw; *nh = ah - halfh;  break;
        default: break;
    }
    if (*nw < WM_MIN_WIDTH)  *nw = WM_MIN_WIDTH;
    if (*nh < WM_MIN_HEIGHT) *nh = WM_MIN_HEIGHT;
}

// which snap region the window's current rect matches, or -1. (satoru)
static int wm_snap_edge_current(const Window* win, int ax, int ay, int aw, int ah) {
    if (!win) return -1;
    int nx, ny, nw, nh;
    for (int e = 0; e <= 7; e++) {
        if (e == 3) continue;
        wm_snap_rect(e, ax, ay, aw, ah, &nx, &ny, &nw, &nh);
        if (win->x == nx && win->y == ny && win->w == nw && win->h == nh) return e;
    }
    return -1;
}

// edge under a dragged pointer, inside the output it points at. quarters win
// over a single edge when the pointer is in a corner. (satoru)
static int wm_snap_edge_at(int mx, int my, int* ax, int* ay, int* aw, int* ah) {
    int oi = 0;
    if (Outputs::Multi()) {
        oi = Outputs::At(mx, my);
        if (oi < 0) oi = Outputs::Nearest(mx, my);
    }
    wm_area_of(oi, ax, ay, aw, ah);
    int ox, oy, ow, oh;
    wm_output_rect(oi, &ox, &oy, &ow, &oh);
    const int EDGE = 14;
    bool left   = mx <= ox + EDGE;
    bool right  = mx >= ox + ow - 1 - EDGE;
    bool top    = my <= oy + EDGE;
    bool bottom = my >= oy + oh - 1 - EDGE;
    if (top && left)     return 4;
    if (top && right)    return 5;
    if (bottom && left)  return 6;
    if (bottom && right) return 7;
    if (top)    return 2;
    if (left)   return 0;
    if (right)  return 1;
    return -1;
}

// ── per-app window geometry memory ───────────────────────────────────
// a small side table keyed by a normalized app name (or "app:<icon>" once a
// client reports a stable id), persisted to /etc/kurono/wm_geometry.conf. the
// window struct is not enlarged for this. (satoru)
#define WM_GEOM_MAX 32
#define WM_GEOM_KEY 40
struct WMGeomEntry {
    char key[WM_GEOM_KEY];
    int x, y, w, h;
    bool used;
};
static WMGeomEntry wm_geoms[WM_GEOM_MAX];
static char        wm_win_key[WM_MAX_WINDOWS][WM_GEOM_KEY];
static bool        wm_geom_loaded = false;
static bool        wm_geom_dirty = false;
static unsigned int wm_geom_saved_ms = 0;
static const char* wm_geom_path = "/etc/kurono/wm_geometry.conf";

static void wm_geom_key_norm(const char* s, char* out, int max) {
    int n = 0;
    if (!s) s = "";
    while (*s && n < max - 1) {
        char c = *s++;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        bool keep = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '.' || c == '-' || c == '_';
        if (!keep) {
            if (c == ' ' || c == '/' || c == '\\' || c == ':' || c == '(' ||
                c == ')' || c == '[' || c == ']') {
                if (n > 0 && out[n - 1] != '_') out[n++] = '_';
            }
            continue;
        }
        out[n++] = c;
    }
    while (n > 0 && out[n - 1] == '_') n--;
    if (n == 0) {
        const char* d = "window";
        for (int i = 0; d[i] && n < max - 1; i++) out[n++] = d[i];
    }
    out[n] = 0;
}

static void wm_geom_key_icon(int icon, char* out, int max) {
    const char* pfx = "app:";
    int n = 0;
    for (int i = 0; pfx[i] && n < max - 1; i++) out[n++] = pfx[i];
    char rev[12]; int r = 0;
    int v = icon;
    if (v < 0) v = 0;
    if (v == 0) rev[r++] = '0';
    while (v > 0 && r < 11) { rev[r++] = (char)('0' + v % 10); v /= 10; }
    while (r > 0 && n < max - 1) out[n++] = rev[--r];
    out[n] = 0;
}

static bool wm_geom_streq(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == 0 && *b == 0;
}

static int wm_geom_find(const char* key) {
    if (!key || !key[0]) return -1;
    for (int i = 0; i < WM_GEOM_MAX; i++) {
        if (wm_geoms[i].used && wm_geom_streq(wm_geoms[i].key, key)) return i;
    }
    return -1;
}

static void wm_geom_store(const char* key, int x, int y, int w, int h) {
    if (!key || !key[0]) return;
    if (w < WM_MIN_WIDTH)  w = WM_MIN_WIDTH;
    if (h < WM_MIN_HEIGHT) h = WM_MIN_HEIGHT;
    int idx = wm_geom_find(key);
    if (idx < 0) {
        for (int i = 0; i < WM_GEOM_MAX; i++) {
            if (!wm_geoms[i].used) { idx = i; wm_geoms[i].used = true; break; }
        }
    }
    if (idx < 0) return;
    WMGeomEntry& e = wm_geoms[idx];
    if (e.used && e.x == x && e.y == y && e.w == w && e.h == h) return;
    int kn = 0;
    while (key[kn] && kn < WM_GEOM_KEY - 1) { e.key[kn] = key[kn]; kn++; }
    e.key[kn] = 0;
    e.x = x; e.y = y; e.w = w; e.h = h;
    e.used = true;
    wm_geom_dirty = true;
}

static int wm_geom_read_int(const char* b, int& i, int n) {
    while (i < n && (b[i] == ' ' || b[i] == '\t')) i++;
    bool neg = false;
    if (i < n && (b[i] == '-' || b[i] == '+')) { neg = b[i] == '-'; i++; }
    int v = 0;
    while (i < n && b[i] >= '0' && b[i] <= '9') { v = v * 10 + (b[i] - '0'); i++; }
    return neg ? -v : v;
}

static void wm_geom_load() {
    if (wm_geom_loaded) return;
    wm_geom_loaded = true;
    static char buf[4096];
    int n = KVFS::ReadFile(wm_geom_path, buf, (uint32_t)sizeof(buf) - 1);
    if (n <= 0) return;
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    buf[n] = 0;
    int i = 0;
    while (i < n) {
        while (i < n && (buf[i] == '\r' || buf[i] == '\n' || buf[i] == ' ' || buf[i] == '\t')) i++;
        if (i >= n) break;
        if (buf[i] == '#') { while (i < n && buf[i] != '\n') i++; continue; }
        char key[WM_GEOM_KEY]; int k = 0;
        while (i < n && buf[i] != ' ' && buf[i] != '\t' && buf[i] != '\r' && buf[i] != '\n' &&
               k < WM_GEOM_KEY - 1) key[k++] = buf[i++];
        key[k] = 0;
        int x = wm_geom_read_int(buf, i, n);
        int y = wm_geom_read_int(buf, i, n);
        int w = wm_geom_read_int(buf, i, n);
        int h = wm_geom_read_int(buf, i, n);
        if (k > 0 && w >= WM_MIN_WIDTH && h >= WM_MIN_HEIGHT && w < 100000 && h < 100000)
            wm_geom_store(key, x, y, w, h);
        while (i < n && buf[i] != '\n') i++;
    }
    wm_geom_dirty = false;
}

static void wm_geom_append_int(char*& p, char* end, int v) {
    if (p >= end - 1) return;
    if (v < 0) { *p++ = '-'; v = -v; }
    char rev[12]; int r = 0;
    if (v == 0) rev[r++] = '0';
    while (v > 0 && r < 11) { rev[r++] = (char)('0' + v % 10); v /= 10; }
    while (r > 0 && p < end - 1) *p++ = rev[--r];
    *p = 0;
}

static void wm_geom_save() {
    static char out[4096];
    char* p = out;
    char* end = out + sizeof(out);
    const char* hdr = "# kurono window geometry: key x y w h\n";
    for (int i = 0; hdr[i] && p < end - 1; i++) *p++ = hdr[i];
    *p = 0;
    for (int i = 0; i < WM_GEOM_MAX; i++) {
        if (!wm_geoms[i].used) continue;
        const char* k = wm_geoms[i].key;
        while (*k && p < end - 2) *p++ = *k++;
        if (p < end - 1) *p++ = ' ';
        wm_geom_append_int(p, end, wm_geoms[i].x);
        if (p < end - 1) *p++ = ' ';
        wm_geom_append_int(p, end, wm_geoms[i].y);
        if (p < end - 1) *p++ = ' ';
        wm_geom_append_int(p, end, wm_geoms[i].w);
        if (p < end - 1) *p++ = ' ';
        wm_geom_append_int(p, end, wm_geoms[i].h);
        if (p < end - 1) *p++ = '\n';
    }
    *p = 0;
    KVFS::WriteString(wm_geom_path, out);
    wm_geom_dirty = false;
    wm_geom_saved_ms = Timer::GetRealMs();
}

static void wm_geom_flush(unsigned int now) {
    if (!wm_geom_dirty) return;
    if (wm_geom_saved_ms && now - wm_geom_saved_ms < 1500u) return;
    wm_geom_save();
}

// remember the window's normal (pre-snap / pre-maximize) rect so the next
// launch of the same app opens there. (satoru)
static void wm_geom_remember(Window* win, bool use_current = false) {
    if (!win) return;
    int slot = (int)(win - WindowManager::GetWindows());
    if (slot < 0 || slot >= WM_MAX_WINDOWS || !wm_win_key[slot][0]) return;
    int x = win->x, y = win->y, w = win->w, h = win->h;
    if (use_current) {
        // a drag or resize that left a snap keeps its new free rect. (satoru)
    } else if (win->state == WIN_FULLSCREEN) {
        x = win->fs_x; y = win->fs_y; w = win->fs_w; h = win->fs_h;
    } else if (win->state == WIN_MAXIMIZED) {
        x = win->saved_x; y = win->saved_y; w = win->saved_w; h = win->saved_h;
    } else if (win->snap_saved) {
        x = win->snap_saved_x; y = win->snap_saved_y;
        w = win->snap_saved_w; h = win->snap_saved_h;
    }
    if (w < WM_MIN_WIDTH || h < WM_MIN_HEIGHT || w > 100000 || h > 100000) return;
    wm_geom_store(wm_win_key[slot], x, y, w, h);
}

// apply a saved rect, clamped to the work area of its own center. used at
// create time and when a wayland app id arrives before the first frame. (satoru)
bool WindowManager::ApplySavedGeometry(Window* win, const char* key) {
    if (!win || !key) return false;
    int idx = wm_geom_find(key);
    if (idx < 0) return false;
    int x = wm_geoms[idx].x, y = wm_geoms[idx].y;
    int w = wm_geoms[idx].w, h = wm_geoms[idx].h;
    int ax, ay, aw, ah;
    wm_area_at(x + w / 2, y + h / 2, &ax, &ay, &aw, &ah);
    if (w > aw) w = aw;
    if (h > ah) h = ah;
    if (w < WM_MIN_WIDTH)  w = WM_MIN_WIDTH;
    if (h < WM_MIN_HEIGHT) h = WM_MIN_HEIGHT;
    if (x < ax) x = ax;
    if (y < ay) y = ay;
    if (x + w > ax + aw) x = ax + aw - w;
    if (y + h > ay + ah) y = ay + ah - h;
    wm_damage_window(win);
    win->x = x; win->y = y; win->w = w; win->h = h;
    win->saved_x = x; win->saved_y = y; win->saved_w = w; win->saved_h = h;
    if (!win->snap_saved) {
        win->snap_saved_x = x; win->snap_saved_y = y;
        win->snap_saved_w = w; win->snap_saved_h = h;
    }
    WindowManager::UpdateContentArea(win);
    WindowManager::UpdateWindowMonitor(win);
    win->dirty = true;
    wm_damage(win->x, win->y, win->w, win->h);
    return true;
}

// ── alt-tab live thumbnails ──────────────────────────────────────────
// one downscaled argb buffer per window, refreshed while the switcher is up.
// grabs happen right after a window is composited, before any window above it
// is drawn, so a covered window still gets a clean thumbnail. (satoru)
struct WMTabThumb {
    uint32_t* px;
    int w, h;
    int id;
    bool valid;
};
static WMTabThumb wm_tab_thumbs[WM_MAX_WINDOWS];
static bool wm_tab_open = false;
static int  wm_tab_ids[WM_MAX_WINDOWS];
static int  wm_tab_count = 0;
static int  wm_tab_sel = 0;
static int  wm_tab_monitor = 0;
static unsigned int wm_tab_open_ms = 0;
static unsigned int wm_tab_sel_ms = 0;
static int  wm_tab_ax = 0, wm_tab_ay = 0, wm_tab_aw = 0, wm_tab_ah = 0;

static void wm_tab_thumb_free_all() {
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (wm_tab_thumbs[i].px) KernelHeap::Free(wm_tab_thumbs[i].px);
        wm_tab_thumbs[i].px = nullptr;
        wm_tab_thumbs[i].w = wm_tab_thumbs[i].h = 0;
        wm_tab_thumbs[i].id = -1;
        wm_tab_thumbs[i].valid = false;
    }
}

// keep only the thumbnails of minimized windows: they are not composited, so
// the grab taken as they minimized is the only preview the switcher has. (satoru)
static void wm_tab_thumb_trim() {
    Window* ws = WindowManager::GetWindows();
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        WMTabThumb& t = wm_tab_thumbs[i];
        bool keep = t.valid && t.px && ws[i].state == WIN_MINIMIZED && ws[i].id == t.id;
        if (keep) continue;
        if (t.px) KernelHeap::Free(t.px);
        t.px = nullptr;
        t.w = t.h = 0;
        t.id = -1;
        t.valid = false;
    }
}

static void wm_tab_thumb_grab(Window* win, bool force) {
    if ((!wm_tab_open && !force) || !win) return;
    if (win->state == WIN_CLOSED || win->state == WIN_MINIMIZED || !win->visible) return;
    if (win->fx_mode != FX_NONE) return;   // mid-motion, keep the last clean grab (satoru)
    int slot = (int)(win - WindowManager::GetWindows());
    if (slot < 0 || slot >= WM_MAX_WINDOWS || win->w <= 0 || win->h <= 0) return;
    if (Graphics::GetBpp() != 32 || !Graphics::GetBuffer()) return;

    const int maxw = 256, maxh = 160;
    int tw, th;
    if ((long long)win->w * maxh >= (long long)win->h * maxw) {
        tw = maxw;
        th = (int)(((long long)win->h * maxw) / win->w);
    } else {
        th = maxh;
        tw = (int)(((long long)win->w * maxh) / win->h);
    }
    if (tw < 8) tw = 8;
    if (th < 8) th = 8;
    WMTabThumb& t = wm_tab_thumbs[slot];
    if (t.w != tw || t.h != th || !t.px) {
        if (t.px) KernelHeap::Free(t.px);
        t.px = (uint32_t*)KernelHeap::Alloc((size_t)tw * (size_t)th * 4u);
        t.w = t.px ? tw : 0;
        t.h = t.px ? th : 0;
        t.valid = false;
    }
    if (!t.px) return;
    uint32_t* src = (uint32_t*)Graphics::GetBuffer();
    int pitch = (int)(Graphics::GetPitch() / 4u);
    int scrw = Graphics::GetWidth(), scrh = Graphics::GetHeight();
    uint32_t* dst = t.px;
    for (int j = 0; j < th; j++) {
        int sy = win->y + (int)(((long long)j * win->h) / th);
        uint32_t* row = dst + (size_t)j * (size_t)tw;
        for (int i = 0; i < tw; i++) {
            int sx = win->x + (int)(((long long)i * win->w) / tw);
            row[i] = ((unsigned)sx < (unsigned)scrw && (unsigned)sy < (unsigned)scrh)
                     ? (src[(size_t)sy * pitch + sx] | 0xFF000000u) : 0xFF101018u;
        }
    }
    t.id = win->id;
    t.valid = true;
}

static int wm_tab_gather(int* ids) {
    int n = 0;
    Window* ws = WindowManager::GetWindows();
    for (int i = 0; i < WM_MAX_WINDOWS && n < 16; i++) {
        if (ws[i].state == WIN_CLOSED) continue;
        ids[n++] = ws[i].id;
    }
    for (int i = 1; i < n; i++) {
        int id = ids[i];
        Window* w = WindowManager::GetWindow(id);
        int z = w ? w->z_order : -1;
        int j = i - 1;
        while (j >= 0) {
            Window* o = WindowManager::GetWindow(ids[j]);
            int oz = o ? o->z_order : -1;
            if (oz >= z) break;
            ids[j + 1] = ids[j];
            j--;
        }
        ids[j + 1] = id;
    }
    return n;
}

static int wm_tab_index_of(const int* ids, int n, int id) {
    for (int i = 0; i < n; i++) if (ids[i] == id) return i;
    return -1;
}

static void wm_tab_fit_text(const char* src, float pxh, int maxw, char* out, int maxn) {
    if (!src) src = "";
    int n = 0;
    while (src[n] && n < maxn - 1) { out[n] = src[n]; n++; }
    out[n] = 0;
    if (FontTTF::MeasureText(pxh, out, false) <= maxw) return;
    while (n > 0) {
        out[n--] = 0;
        if (n > 3) {
            out[n] = 0;
            if (FontTTF::MeasureText(pxh, out, false) + 18 <= maxw) break;
        }
    }
    if (n > maxn - 4) n = maxn - 4;
    if (n < 0) n = 0;
    out[n++] = '.';
    out[n++] = '.';
    out[n++] = '.';
    out[n] = 0;
}

// ── show-desktop state ───────────────────────────────────────────────
static bool wm_show_desktop = false;
static unsigned long long wm_sd_mask[WM_MAX_WINDOWS / 64];   // show-desktop set, one bit per slot (satoru)
static int wm_sd_focus = -1;

// kurono ui color scheme - modern glassmorphism
// Static fallbacks used when UIConfig is not yet available.
// At render time, focused colours read the LIVE accent from the kss stylesheet
// (the "window" rule's P_ACCENT), so a kj script doing kss.transition+kss.set on
// "window"/"accent" eases the real window borders + titlebar accent on screen.
// LiveAccent() falls back to the theme accent before Sheet::Init, which itself
// seeds from theme.accent - so behaviour is unchanged until something animates
// the accent. (satoru)
static uint32_t wm_get_accent(){
    return KSS::Sheet::LiveAccent();
}
#define COL_TITLE_BG       0xFF1C1C2E  // frosted dark
#define COL_TITLE_FOCUSED  0xFF22223A  // focused: base - overlaid with accent
#define COL_TITLE_GRAD     0xFF2A2A48  // gradient bottom for focused
#define COL_TITLE_TEXT     0xFFF0F0F5
#define COL_TITLE_TEXT_DIM 0xFF888898
#define COL_CLOSE_BTN      0xFFFF5F57  // macos red
#define COL_MAX_BTN        0xFF28C840  // macos green
#define COL_MIN_BTN        0xFFFFBD2E  // macos amber
#define COL_BORDER         0xFF303050
#define COL_SHADOW         0xFF08080C
#define COL_WIN_BG         0xFF121218  // dark background
#define COL_SEPARATOR      0xFF2A2A40  // titlebar/content separator

void WindowManager::Init(int sw, int sh) {
    screen_width = sw;
    screen_height = sh;
    wm_desktop_x = 0;
    wm_desktop_y = 0;
    wm_desktop_w = sw;
    wm_desktop_h = sh - 40;
    if (wm_desktop_h < WM_MIN_HEIGHT) wm_desktop_h = sh;
    window_count = 0;
    next_id = 1;
    focused_id = -1;
    current_action = WM_NONE;

    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        windows[i].state = WIN_CLOSED;
        windows[i].id = 0;
        windows[i].anim_kind = 0;
        windows[i].snap_px = nullptr;
        windows[i].catch_kind = 0;
        windows[i].fx_old_px = nullptr;
        wm_fx_init(&windows[i]);
        windows[i].had_last = false;
        windows[i].visible = false;
        windows[i].focused = false;
        windows[i].monitor_id = 0;
    }
    wm_ctx_open = false;
    wm_ctx_win_id = -1;
    wm_input_capture_id = -1;
    wm_last_drag_ms = 0;
    // reset drag-snapshot state; the buffer (if any) is re-sized on next use
    // via its geometry check, so we just drop readiness here. (satoru)
    InvalidateDragBackdrop();
    for (int i = 0; i < WM_MAX_WINDOWS; i++) wm_win_key[i][0] = 0;
    wm_tab_open = false;
    wm_tab_count = 0;
    wm_tab_sel = 0;
    wm_tab_monitor = 0;
    wm_tab_thumb_free_all();
    wm_snap_preview_clear();
    wm_show_desktop = false;
    for (int q = 0; q < WM_MAX_WINDOWS / 64; q++) wm_sd_mask[q] = 0;
    wm_sd_focus = -1;
    wm_geom_dirty = false;
    wm_geom_saved_ms = 0;
    wm_geom_load();

    SerialLogger::Log("WindowManager: Initialized\r\n");
}

Window* WindowManager::CreateWindow(const char* title, int x, int y, int w, int h) {
    // find free slot
    int slot = -1;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (windows[i].state == WIN_CLOSED) { slot = i; break; }
    }
    if (slot == -1) return nullptr;

    Window* win = &windows[slot];
    win->id = next_id++;
    if (next_id < 1) next_id = 1;     // wrap guard
    TaskMem::BindWindow(win->id);   // a window opened inside an app scope belongs to that app (satoru)
    wmcpy(win->title, title, 64);
    if (w < WM_MIN_WIDTH)  w = WM_MIN_WIDTH;
    if (h < WM_MIN_HEIGHT) h = WM_MIN_HEIGHT;
    // a window that asked to be centred remembers its app's last position. a
    // dialog or explicitly placed window keeps the caller's rect. (satoru)
    wm_geom_key_norm(title, wm_win_key[slot], WM_GEOM_KEY);
    bool restored = false;
    if (x < 0 && y < 0) {
        int gi = wm_geom_find(wm_win_key[slot]);
        if (gi >= 0) {
            x = wm_geoms[gi].x; y = wm_geoms[gi].y;
            w = wm_geoms[gi].w; h = wm_geoms[gi].h;
            restored = true;
        }
    }
    // new windows open on the output under the cursor (the primary's work area
    // on a single screen). (satoru)
    int nax, nay, naw, nah;
    if (restored) wm_area_at(x + w / 2, y + h / 2, &nax, &nay, &naw, &nah);
    else          wm_area_cursor(&nax, &nay, &naw, &nah);
    if (w > naw)   w = naw;
    if (h > nah)   h = nah;
    if (w < WM_MIN_WIDTH)  w = WM_MIN_WIDTH;
    if (h < WM_MIN_HEIGHT) h = WM_MIN_HEIGHT;
    if (!restored) {
        if (x < 0) x = nax + (naw - w) / 2;
        if (y < 0) y = nay + (nah - h) / 2;
    }
    if (x < nax) x = nax;
    if (y < nay) y = nay;
    if (x + w > nax + naw) x = nax + naw - w;
    if (y + h > nay + nah) y = nay + nah - h;
    win->x = x; win->y = y;
    win->w = w; win->h = h;
    win->saved_x = x; win->saved_y = y;
    win->saved_w = w; win->saved_h = h;
    win->snap_saved_x = x; win->snap_saved_y = y;   // snap restore rect (satoru)
    win->snap_saved_w = w; win->snap_saved_h = h;
    win->snap_saved = false;
    win->state = WIN_NORMAL;
    win->visible = true;
    win->focused = false;
    win->resizable = true;
    win->closable = true;
    win->has_titlebar = true;
    win->z_order = window_count;
    win->bg_color = COL_WIN_BG;
    win->title_color = COL_TITLE_BG;
    win->border_color = COL_BORDER;
    win->render = nullptr;
    win->input = nullptr;
    win->on_close = nullptr;
    win->user_data = nullptr;
    win->dirty = true;

    // compositor: grow out of the taskbar tile, default alpha
    win->anim_kind        = comp_animations_enabled && !comp_reduced_motion ? 1 : 0;
    win->anim_start_ms    = Timer::GetRealMs();
    win->anim_duration_ms = wm_motion_dur(WM_OPEN_MS);
    wm_snap_release(win);
    win->open_ox          = -1;       // resolved from the tile on the first frame
    win->open_oy          = -1;
    win->open_tint        = COL_WIN_BG;
    win->catch_kind       = 0;
    win->catch_ms         = 0;
    win->alpha            = (unsigned char)comp_default_alpha;
    win->anchor_x         = (short)(x + w / 2);
    win->anchor_y         = (short)(screen_height - 22);
    win->tb_hover_btn     = -1;
    win->tb_press_ms      = 0;
    win->last_x = x; win->last_y = y;
    win->last_w = w; win->last_h = h;
    win->had_last = false;
    win->monitor_id = 0;
    win->app_icon = -1;               // derive from title unless pinned (satoru)
    win->fs_prev_state = WIN_NORMAL;
    win->fs_prev_tb = true;
    win->fs_x = x; win->fs_y = y; win->fs_w = w; win->fs_h = h;
    wm_fx_old_release(win);
    wm_fx_init(win);
    if (wm_spring && win->anim_kind == 1) {
        // spring open: rise out of a slightly smaller, transparent copy of
        // itself, drawn from a capture taken on its first frame. (satoru)
        win->anim_kind = 0;
        win->fx_mode = FX_OPEN;
        win->fx_cap = true;
        WMMotion::SpringSet(win->fx_sc, 0.90f); win->fx_sc.t = 1.0f;
        WMMotion::SpringSet(win->fx_op, 0.0f);  win->fx_op.t = 1.0f;
        WMMotion::SpringSet(win->fx_el, 1.0f);
        win->fx_us = wm_now_us();
    }

    UpdateContentArea(win);
    UpdateWindowMonitor(win);   // assign to the output under its center (satoru)
    wm_damage_window(win);

    if (window_count < WM_MAX_WINDOWS) window_count++;
    Focus(win->id);

    return win;
}

void WindowManager::CloseWindow(int id) {
    Window* win = GetWindow(id);
    if (!win) return;
    int slot = (int)(win - windows);
    if (slot >= 0 && slot < WM_MAX_WINDOWS) wm_sd_mask[slot / 64] &= ~(1ull << (slot % 64));
    if (wm_spring && wm_anim_on()) {
        if (win->fx_mode == FX_CLOSE) return;
        // spring close: shrink a little and fade from a capture. the window
        // stops taking input and hands focus on at once. (satoru)
        if (win->fx_mode == FX_MIN || win->fx_mode == FX_UNMIN || !win->snap_px) win->fx_cap = true;
        if (win->fx_mode == FX_MORPH) win->fx_cap = true;
        win->fx_mode = FX_CLOSE;
        win->anim_kind = 2;
        win->anim_duration_ms = 0;
        win->fx_sc.t = 0.92f;
        win->fx_op.t = 0.0f;
        if (!win->fx_us) win->fx_us = wm_now_us();
        if (focused_id == id) {
            win->focused = false;
            focused_id = -1;
            int nf = wm_next_focus(windows, id);
            if (nf > 0) Focus(nf);
        }
        if (wm_input_capture_id == id) wm_input_capture_id = -1;
        if (action_window_id == id) { action_window_id = -1; current_action = WM_NONE; }
        Graphics::MarkUIDirty();
        return;
    }
    // Defer the actual transition to CLOSED until the close animation
    // finishes; TickAnimations() will retire the window then.  Skip
    // the defer if animations are off, otherwise the user sees no
    // exit feedback.
    if (comp_animations_enabled && !comp_reduced_motion &&
        comp_anim_duration_ms > 0 && win->anim_kind != 2) {
        win->anim_kind        = 2;            // close
        win->anim_start_ms    = Timer::GetRealMs();
        win->anim_duration_ms = wm_motion_dur(WM_CLOSE_MS);
        return;
    }
    wm_damage_window(win);
    wm_snap_release(win);
    win->state = WIN_CLOSED;
    win->visible = false;
    win->had_last = false;
    if (win->on_close) { void (*cb)(Window*) = win->on_close; win->on_close = nullptr; cb(win); }
    if (focused_id == id) focused_id = -1;
    if (wm_input_capture_id == id) wm_input_capture_id = -1;
    if (action_window_id == id) {
        action_window_id = -1;
        current_action = WM_NONE;
    }
}

void WindowManager::CloseAll() {
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (windows[i].state != WIN_CLOSED) {
            wm_damage_window(&windows[i]);
            windows[i].state = WIN_CLOSED;
            if (windows[i].on_close) { void (*cb)(Window*) = windows[i].on_close; windows[i].on_close = nullptr; cb(&windows[i]); }
            windows[i].visible = false;
            windows[i].had_last = false;
            windows[i].anim_kind = 0;
            wm_snap_release(&windows[i]);
            wm_fx_old_release(&windows[i]);
            windows[i].fx_mode = 0;
        }
    }
    focused_id = -1;
    wm_input_capture_id = -1;
    action_window_id = -1;
    current_action = WM_NONE;
    wm_tab_open = false;
    wm_tab_thumb_free_all();
    wm_snap_preview_clear();
    wm_show_desktop = false;
    for (int q = 0; q < WM_MAX_WINDOWS / 64; q++) wm_sd_mask[q] = 0;
    wm_sd_focus = -1;
}

// a close the USER asked for (titlebar button, window menu): a wayland window
// asks its client first (xdg_toplevel.close) and goes when the client does - an
// app closed only on the wm side kept running windowless. (satoru)
void WindowManager::UserCloseWindow(int id) {
    if (WaylandServer::RequestClose(id)) return;
    CloseWindow(id);
}

void WindowManager::DestroyWindow(int id) {
    CloseWindow(id);
}

Window* WindowManager::GetWindow(int id) {
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (windows[i].id == id && windows[i].state != WIN_CLOSED)
            return &windows[i];
    }
    return nullptr;
}

Window* WindowManager::GetFocusedWindow() {
    return GetWindow(focused_id);
}

void WindowManager::Minimize(int id) {
    Window* win = GetWindow(id);
    if (!win) return;
    // the alt-tab preview of a minimized window is its frame from right now. (satoru)
    if (win->state != WIN_MINIMIZED) wm_tab_thumb_grab(win, true);
    if (wm_spring && wm_anim_on()) {
        if (win->state == WIN_MINIMIZED || win->fx_mode == FX_MIN || win->fx_mode == FX_CLOSE) return;
        // spring minimize: the genie drains the window into its dock tile.
        // a restore mid-flight just retargets the same spring. (satoru)
        if (win->fx_mode != FX_UNMIN || !win->snap_px) win->fx_cap = true;
        if (win->fx_mode == FX_OPEN || win->fx_mode == FX_MORPH || win->fx_mode == FX_NONE) {
            WMMotion::SpringSet(win->fx_gn, 0.0f);
            WMMotion::SpringSet(win->fx_sc, 1.0f);
            WMMotion::SpringSet(win->fx_op, 1.0f);
        }
        win->fx_rect = false;
        wm_fx_old_release(win);
        win->fx_mode = FX_MIN;
        win->fx_gn.t = 1.0f;
        win->fx_caught = false;
        win->anim_kind = 3;
        win->anim_duration_ms = 0;
        if (!win->fx_us) win->fx_us = wm_now_us();
        if (focused_id == id) {
            win->focused = false;
            focused_id = -1;
            int nf = wm_next_focus(windows, id);
            if (nf > 0) Focus(nf);
        }
        if (action_window_id == id) { action_window_id = -1; current_action = WM_NONE; }
        wm_damage_window(win);
        return;
    }
    if (comp_animations_enabled && !comp_reduced_motion &&
        comp_anim_duration_ms > 0 && win->anim_kind != 3) {
        // defer hide until the pour into the taskbar tile completes; the tile
        // catches the window 300 ms in, as the first rows arrive.
        unsigned int now = Timer::GetRealMs();
        win->anim_kind        = 3;
        win->anim_start_ms    = now;
        win->anim_duration_ms = wm_motion_dur(WM_MIN_MS);
        win->catch_kind       = 1;
        win->catch_ms         = now + wm_motion_dur(300);
        return;
    }
    wm_damage_window(win);
    win->state = WIN_MINIMIZED;
    win->visible = false;
    win->had_last = false;
}

void WindowManager::Unminimize(int id) {
    Window* win = GetWindow(id);
    if (!win) return;
    if (wm_show_desktop) {
        int slot = (int)(win - windows);
        if (slot >= 0 && slot < WM_MAX_WINDOWS) wm_sd_mask[slot / 64] &= ~(1ull << (slot % 64));
    }
    bool animate = comp_animations_enabled && !comp_reduced_motion && comp_anim_duration_ms > 0;
    unsigned int now = Timer::GetRealMs();
    if (wm_spring && animate) {
        // spring restore: the genie runs backwards from wherever it is. (satoru)
        bool was_min = win->state == WIN_MINIMIZED;
        win->state   = WIN_NORMAL;
        win->visible = true;
        win->dirty   = true;
        win->had_last = false;
        if (was_min) {
            WMMotion::SpringSet(win->fx_gn, 1.0f);
            if (!win->snap_px) win->fx_cap = true;
        } else if (win->fx_mode != FX_MIN && win->fx_mode != FX_UNMIN) {
            return;   // not minimized, nothing to restore (satoru)
        }
        win->fx_mode = FX_UNMIN;
        win->fx_gn.t = 0.0f;
        win->fx_rect = false;
        win->anim_kind = 5;
        win->anim_duration_ms = 0;
        win->catch_kind = 2;
        win->catch_ms = now;
        if (!win->fx_us) win->fx_us = wm_now_us();
        UpdateContentArea(win);
        Focus(id);
        wm_damage_window(win);
        return;
    }
    win->state   = WIN_NORMAL;
    win->visible = true;
    win->dirty   = true;
    win->had_last = false;
    if (animate && win->snap_px) {
        // rise out of the tile from the snapshot minimize kept.
        win->anim_kind        = 5;
        win->anim_start_ms    = now;
        win->anim_duration_ms = wm_motion_dur(WM_UNMIN_MS);
        win->catch_kind       = 2;
        win->catch_ms         = now;
    } else if (animate) {
        // minimized without a capture (animations were off): grow like an open.
        win->anim_kind        = 1;
        win->anim_start_ms    = now;
        win->anim_duration_ms = wm_motion_dur(WM_OPEN_MS);
        win->open_ox = win->open_oy = -1;
    } else {
        win->anim_kind = 0;
        wm_snap_release(win);
    }
    UpdateContentArea(win);
    wm_damage_window(win);
}

void WindowManager::Maximize(int id) {
    Window* win = GetWindow(id);
    if (!win) return;
    if (win->state != WIN_MAXIMIZED) {
        win->saved_x = win->x; win->saved_y = win->y;
        win->saved_w = win->w; win->saved_h = win->h;
    }
    const int mox = win->x, moy = win->y, mow = win->w, moh = win->h;   // morph origin (satoru)
    wm_damage_window(win);
    // maximize on the output the window is on (work area of that output).
    // the monitor id, not the titlebar point, decides, so a window straddling
    // a seam still fills the screen it belongs to. (satoru)
    int max_ax, max_ay, max_aw, max_ah;
    wm_window_area(win, &max_ax, &max_ay, &max_aw, &max_ah);
    win->x = max_ax; win->y = max_ay;
    win->w = max_aw; win->h = max_ah;
    win->state = WIN_MAXIMIZED;
    win->visible = true;
    win->dirty = true;
    UpdateContentArea(win);
    wm_damage(win->x, win->y, win->w, win->h);
    wm_geom_remember(win);
    wm_fx_morph(win, mox, moy, mow, moh, win->has_titlebar);
}

void WindowManager::Restore(int id) {
    Window* win = GetWindow(id);
    if (!win) return;
    const int mox = win->x, moy = win->y, mow = win->w, moh = win->h;   // morph origin (satoru)
    wm_damage_window(win);
    int rx = win->saved_x, ry = win->saved_y;
    int rw = win->saved_w, rh = win->saved_h;
    if (rw < WM_MIN_WIDTH)  rw = WM_MIN_WIDTH;
    if (rh < WM_MIN_HEIGHT) rh = WM_MIN_HEIGHT;
    int rax, ray, raw_, rah;
    wm_window_area(win, &rax, &ray, &raw_, &rah);   // restore on the output it is on (satoru)
    if (rw > raw_)  rw = raw_;
    if (rh > rah)   rh = rah;
    if (rx < rax) rx = rax;
    if (ry < ray) ry = ray;
    if (rx + rw > rax + raw_) rx = rax + raw_ - rw;
    if (ry + rh > ray + rah) ry = ray + rah - rh;
    win->x = rx; win->y = ry;
    win->w = rw; win->h = rh;
    win->state = WIN_NORMAL;
    win->visible = true;
    win->dirty = true;
    wm_geom_remember(win);
    if (wm_spring) {
        UpdateContentArea(win);
        wm_damage(win->x, win->y, win->w, win->h);
        wm_fx_morph(win, mox, moy, mow, moh, win->has_titlebar);
        return;
    }
    if (comp_animations_enabled && !comp_reduced_motion &&
        comp_anim_duration_ms > 0) {
        win->anim_kind        = 4;            // restore
        win->anim_start_ms    = Timer::GetRealMs();
        win->anim_duration_ms = (unsigned short)comp_anim_duration_ms;
    }
    UpdateContentArea(win);
    wm_damage(win->x, win->y, win->w, win->h);
}

void WindowManager::ToggleMaximize(int id) {
    Window* win = GetWindow(id);
    if (!win) return;
    if (win->state == WIN_FULLSCREEN) { ToggleFullscreen(id); return; }
    if (win->state == WIN_MAXIMIZED) Restore(id);
    else Maximize(id);
}

void WindowManager::ToggleFullscreen(int id) {
    Window* win = GetWindow(id);
    if (!win || win->state == WIN_MINIMIZED) return;
    const int ox = win->x, oy = win->y, ow = win->w, oh = win->h;
    const bool otb = win->has_titlebar;
    wm_damage_window(win);
    if (win->state != WIN_FULLSCREEN) {
        win->fs_x = win->x; win->fs_y = win->y; win->fs_w = win->w; win->fs_h = win->h;
        win->fs_prev_state = (unsigned char)win->state;
        win->fs_prev_tb = win->has_titlebar;
        // the whole output the window is on, over the menu bar and the dock (satoru)
        int mx = 0, my = 0, mw = screen_width, mh = screen_height;
        int oi = win->monitor_id;
        if (!Outputs::Multi() || oi < 0 || oi >= OUTPUT_MAX || !Outputs::IsLive(oi)) oi = 0;
        wm_output_rect(oi, &mx, &my, &mw, &mh);
        win->x = mx; win->y = my; win->w = mw; win->h = mh;
        win->has_titlebar = false;
        win->state = WIN_FULLSCREEN;
    } else {
        win->x = win->fs_x; win->y = win->fs_y; win->w = win->fs_w; win->h = win->fs_h;
        win->has_titlebar = win->fs_prev_tb;
        win->state = win->fs_prev_state == WIN_MAXIMIZED ? WIN_MAXIMIZED : WIN_NORMAL;
    }
    win->visible = true;
    win->dirty = true;
    UpdateContentArea(win);
    UpdateWindowMonitor(win);
    wm_damage(win->x, win->y, win->w, win->h);
    wm_geom_remember(win);
    wm_fx_morph(win, ox, oy, ow, oh, otb);
}

void WindowManager::AnimateGeometry(int id, int ox, int oy, int ow, int oh, bool old_titlebar) {
    Window* win = GetWindow(id);
    if (!win) return;
    wm_fx_morph(win, ox, oy, ow, oh, old_titlebar);
}

// apply a snap edge inside an explicit work area. used by the keyboard path
// and by drag release, which snaps into the output under the pointer. (satoru)
void WindowManager::ApplySnap(Window* win, int edge, int dx, int dy, int dw, int dh) {
    if (!win) return;
    if (dw < WM_MIN_WIDTH)  dw = WM_MIN_WIDTH;
    if (dh < WM_MIN_HEIGHT) dh = WM_MIN_HEIGHT;

    // edge 3 = restore the pre-snap rect, then clear the snap save. (satoru)
    if (edge == 3) {
        if (!win->snap_saved) return;
        const int sox = win->x, soy = win->y, sow = win->w, soh = win->h;   // morph origin (satoru)
        wm_damage_window(win);
        int rx = win->snap_saved_x, ry = win->snap_saved_y;
        int rw = win->snap_saved_w, rh = win->snap_saved_h;
        if (rw < WM_MIN_WIDTH)  rw = WM_MIN_WIDTH;
        if (rh < WM_MIN_HEIGHT) rh = WM_MIN_HEIGHT;
        if (rw > dw) rw = dw;
        if (rh > dh) rh = dh;
        if (rx < dx) rx = dx;
        if (ry < dy) ry = dy;
        if (rx + rw > dx + dw) rx = dx + dw - rw;
        if (ry + rh > dy + dh) ry = dy + dh - rh;
        win->x = rx; win->y = ry;
        win->w = rw; win->h = rh;
        win->state = WIN_NORMAL;
        win->visible = true;
        win->dirty = true;
        win->snap_saved = false;
        UpdateContentArea(win);
        UpdateWindowMonitor(win);
        wm_damage(win->x, win->y, win->w, win->h);
        wm_geom_remember(win);
        wm_fx_morph(win, sox, soy, sow, soh, win->has_titlebar);
        return;
    }

    // save the pre-snap rect once, before the first snap of a run. a maximized
    // or full screen window falls back to its own restore rect so super+down
    // still finds the normal size. (satoru)
    if (!win->snap_saved) {
        if (win->state == WIN_MAXIMIZED) {
            win->snap_saved_x = win->saved_x; win->snap_saved_y = win->saved_y;
            win->snap_saved_w = win->saved_w; win->snap_saved_h = win->saved_h;
        } else if (win->state == WIN_FULLSCREEN) {
            win->snap_saved_x = win->fs_x; win->snap_saved_y = win->fs_y;
            win->snap_saved_w = win->fs_w; win->snap_saved_h = win->fs_h;
        } else {
            win->snap_saved_x = win->x; win->snap_saved_y = win->y;
            win->snap_saved_w = win->w; win->snap_saved_h = win->h;
        }
        win->snap_saved = true;
    }

    int nx, ny, nw, nh;
    wm_snap_rect(edge, dx, dy, dw, dh, &nx, &ny, &nw, &nh);

    const int sox = win->x, soy = win->y, sow = win->w, soh = win->h;   // morph origin (satoru)
    wm_damage_window(win);
    win->x = nx; win->y = ny;
    win->w = nw; win->h = nh;
    // edge 2 maps to the maximized state; the rest are tiled normal. (satoru)
    win->state = (edge == 2) ? WIN_MAXIMIZED : WIN_NORMAL;
    win->visible = true;
    win->dirty = true;
    UpdateContentArea(win);
    UpdateWindowMonitor(win);
    wm_damage(win->x, win->y, win->w, win->h);
    wm_geom_remember(win);
    wm_fx_morph(win, sox, soy, sow, soh, win->has_titlebar);
}

// snap a window to a region of the desktop. reuses the same desktop bounds
// the maximize path uses (x=0, y=GetDesktopY(), w=screen_width,
// h=GetDesktopH()) so snapping never overlaps the taskbar. super+up on a
// vertical half goes to the top quarter, super+down on a half or top quarter
// to the bottom quarter; otherwise those keys keep maximize / restore. (satoru)
void WindowManager::SnapWindow(int win_id, int edge) {
    Window* win = GetWindow(win_id);
    if (!win) return;

    // desktop rect - same area maximize occupies. (satoru)
    int dx, dy, dw, dh;
    wm_window_area(win, &dx, &dy, &dw, &dh);   // snap within its own output (satoru)
    if (dw < WM_MIN_WIDTH)  dw = WM_MIN_WIDTH;
    if (dh < WM_MIN_HEIGHT) dh = WM_MIN_HEIGHT;

    int cur = wm_snap_edge_current(win, dx, dy, dw, dh);
    if (edge == 2) {
        if (cur == 0) edge = 4;
        else if (cur == 1) edge = 5;
    } else if (edge == 3) {
        if (cur == 4 || cur == 0)       edge = 6;
        else if (cur == 5 || cur == 1)  edge = 7;
    }
    ApplySnap(win, edge, dx, dy, dw, dh);
}

// ── drag-to-edge snap preview ────────────────────────────────────────
static int  wm_sp_edge = -1;
static int  wm_sp_win = -1;
static int  wm_sp_x = 0, wm_sp_y = 0, wm_sp_w = 0, wm_sp_h = 0;
static int  wm_sp_ax = 0, wm_sp_ay = 0, wm_sp_aw = 0, wm_sp_ah = 0;

static void wm_snap_preview_clear() {
    if (wm_sp_edge >= 0 && wm_sp_w > 0 && wm_sp_h > 0)
        wm_damage(wm_sp_x - 4, wm_sp_y - 4, wm_sp_w + 8, wm_sp_h + 8);
    wm_sp_edge = -1;
    wm_sp_win = -1;
    wm_sp_w = wm_sp_h = 0;
}

static void wm_snap_preview_update(Window* win, WMAction act, int mx, int my) {
    if (!win || act != WM_DRAG) { wm_snap_preview_clear(); return; }
    int ax, ay, aw, ah;
    int edge = wm_snap_edge_at(mx, my, &ax, &ay, &aw, &ah);
    if (edge < 0) { wm_snap_preview_clear(); return; }
    int nx, ny, nw, nh;
    wm_snap_rect(edge, ax, ay, aw, ah, &nx, &ny, &nw, &nh);
    if (wm_sp_edge == edge && wm_sp_win == win->id &&
        wm_sp_x == nx && wm_sp_y == ny && wm_sp_w == nw && wm_sp_h == nh)
        return;
    if (wm_sp_edge >= 0 && wm_sp_w > 0)
        wm_damage(wm_sp_x - 4, wm_sp_y - 4, wm_sp_w + 8, wm_sp_h + 8);
    wm_sp_edge = edge;
    wm_sp_win = win->id;
    wm_sp_x = nx; wm_sp_y = ny; wm_sp_w = nw; wm_sp_h = nh;
    wm_sp_ax = ax; wm_sp_ay = ay; wm_sp_aw = aw; wm_sp_ah = ah;
    wm_damage(nx - 4, ny - 4, nw + 8, nh + 8);
    Graphics::MarkUIDirty();
}

static const char* wm_snap_edge_name(int edge) {
    switch (edge) {
        case 0: return "Left half";
        case 1: return "Right half";
        case 2: return "Maximize";
        case 4: return "Top left";
        case 5: return "Top right";
        case 6: return "Bottom left";
        case 7: return "Bottom right";
        default: return "";
    }
}

void WindowManager::RenderSnapPreview() {
    if (wm_sp_edge < 0 || wm_sp_win <= 0) return;
    if (current_action != WM_DRAG || action_window_id != wm_sp_win) {
        wm_snap_preview_clear();
        return;
    }
    // the wallpaper repaints the target every frame before this overlay draws,
    // so the translucent fill never accumulates. (satoru)
    wm_damage(wm_sp_x - 4, wm_sp_y - 4, wm_sp_w + 8, wm_sp_h + 8);
    uint32_t accent = wm_get_accent();
    MDraw::RoundRect(wm_sp_x + 1, wm_sp_y + 1, wm_sp_w - 2, wm_sp_h - 2, 14,
                     accent, accent, 46);
    MDraw::RoundOutline(wm_sp_x + 1, wm_sp_y + 1, wm_sp_w - 2, wm_sp_h - 2, 14,
                        accent, 165);
    const char* label = wm_snap_edge_name(wm_sp_edge);
    if (label[0]) {
        int tw = FontTTF::MeasureText(13.0f, label, false);
        if (tw < 0) tw = 0;
        int lx = wm_sp_x + 14, ly = wm_sp_y + 14;
        MDraw::RoundRect(lx, ly, tw + 20, 26, 8, accent, accent, 210);
        FontTTF::DrawText(lx + 10, ly + 5, 13.0f, label, 0xFFFFFFFFu, true);
    }
}

void WindowManager::SnapFocused(int edge) {
    SnapWindow(focused_id, edge);
}

// ── alt-tab switcher ─────────────────────────────────────────────────
void WindowManager::AltTabStep(bool backward) {
    int ids[WM_MAX_WINDOWS];
    int n = wm_tab_gather(ids);
    if (n <= 1) {
        if (wm_tab_open) AltTabCancel();
        return;
    }
    int anchor = wm_tab_open && wm_tab_sel >= 0 && wm_tab_sel < wm_tab_count
                 ? wm_tab_ids[wm_tab_sel] : focused_id;
    int idx = wm_tab_index_of(ids, n, anchor);
    int sel;
    if (idx < 0) sel = backward ? n - 1 : 0;
    else         sel = backward ? (idx + n - 1) % n : (idx + 1) % n;

    for (int i = 0; i < n; i++) wm_tab_ids[i] = ids[i];
    wm_tab_count = n;
    wm_tab_sel = sel;
    wm_tab_sel_ms = Timer::GetRealMs();
    if (!wm_tab_open) {
        wm_tab_open = true;
        wm_tab_open_ms = wm_tab_sel_ms;
        wm_tab_thumb_trim();   // visible windows are grabbed fresh; minimized keep theirs (satoru)
        Window* fw = GetFocusedWindow();
        wm_tab_monitor = fw ? fw->monitor_id : 0;
        if (!Outputs::Multi()) wm_tab_monitor = 0;
        else if (wm_tab_monitor < 0 || wm_tab_monitor >= OUTPUT_MAX ||
                 !Outputs::IsLive(wm_tab_monitor)) wm_tab_monitor = 0;
        wm_output_rect(wm_tab_monitor, &wm_tab_ax, &wm_tab_ay, &wm_tab_aw, &wm_tab_ah);
        if (wm_tab_aw <= 0 || wm_tab_ah <= 0) {
            wm_tab_ax = 0; wm_tab_ay = 0;
            wm_tab_aw = screen_width; wm_tab_ah = screen_height;
        }
    }
    Graphics::MarkUIDirty();
}

void WindowManager::AltTabFinish() {
    if (!wm_tab_open) return;
    int id = (wm_tab_sel >= 0 && wm_tab_sel < wm_tab_count) ? wm_tab_ids[wm_tab_sel] : -1;
    wm_tab_open = false;
    Window* win = GetWindow(id);
    wm_tab_thumb_trim();
    if (win) {
        if (win->state == WIN_MINIMIZED) Unminimize(id);
        else Focus(id);
    }
    wm_damage(wm_tab_ax, wm_tab_ay, wm_tab_aw, wm_tab_ah);
    Graphics::MarkUIDirty();
}

void WindowManager::AltTabCancel() {
    if (!wm_tab_open) return;
    wm_tab_open = false;
    wm_tab_thumb_trim();
    wm_damage(wm_tab_ax, wm_tab_ay, wm_tab_aw, wm_tab_ah);
    Graphics::MarkUIDirty();
}

bool WindowManager::IsTabSwitcherOpen() {
    return wm_tab_open;
}

static void wm_tab_int_str(int v, char* out) {
    if (v < 0) v = 0;
    char rev[12]; int r = 0;
    if (v == 0) rev[r++] = '0';
    while (v > 0 && r < 11) { rev[r++] = (char)('0' + v % 10); v /= 10; }
    int n = 0;
    while (r > 0) out[n++] = rev[--r];
    out[n] = 0;
}

void WindowManager::RenderTabSwitcher() {
    if (!wm_tab_open) return;
    int ids[WM_MAX_WINDOWS];
    int n = 0;
    int sel_local = 0;
    int want = (wm_tab_sel >= 0 && wm_tab_sel < wm_tab_count) ? wm_tab_ids[wm_tab_sel] : -1;
    for (int i = 0; i < wm_tab_count && n < 16; i++) {
        Window* w = GetWindow(wm_tab_ids[i]);
        if (!w) continue;
        if (wm_tab_ids[i] == want) sel_local = n;
        ids[n++] = wm_tab_ids[i];
    }
    if (n <= 0) {
        wm_tab_open = false;
        wm_tab_thumb_free_all();
        wm_damage(wm_tab_ax, wm_tab_ay, wm_tab_aw, wm_tab_ah);
        return;
    }
    if (sel_local >= n) sel_local = n - 1;

    unsigned int now = Timer::GetRealMs();
    float t = Animation::EaseMs(now - wm_tab_open_ms, KSS::Motion::Window, Animation::EaseOutCubic);
    if (!wm_anim_on()) t = 1.0f;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    int fade = (int)(255.0f * t);
    int dim = (int)(96.0f * t);

    const MDraw::Palette& P = MDraw::Pal();
    uint32_t accent = wm_get_accent();
    int ax = wm_tab_ax, ay = wm_tab_ay, aw = wm_tab_aw, ah = wm_tab_ah;
    if (aw <= 0 || ah <= 0) { ax = 0; ay = 0; aw = screen_width; ah = screen_height; }

    if (dim > 0) Graphics::FillRectAlpha(ax, ay, aw, ah, (uint8_t)dim, 0xFF000000u);

    int cols = n <= 4 ? n : (n <= 6 ? 3 : 4);
    if (cols < 1) cols = 1;
    int rows = (n + cols - 1) / cols;
    int gap = 14, pad = 20, title_h = 40, label_h = 24;
    int availw = aw - 80;
    if (availw < 360) { gap = 10; pad = 14; availw = aw - 40; }
    if (availw < 240) availw = aw - 20;
    int cardw = (availw - pad * 2 - gap * (cols - 1)) / cols;
    if (cardw < 120) cardw = 120;
    int thumbh = cardw * 9 / 16;
    if (thumbh > 170) thumbh = 170;
    if (thumbh < 70) thumbh = 70;
    int cardh = thumbh + label_h + 16;
    int panelw = pad * 2 + cols * cardw + gap * (cols - 1);
    int panelh = title_h + rows * cardh + gap * (rows - 1) + pad;
    int maxh = ah - 80;
    if (panelh > maxh) {
        int over = panelh - maxh;
        thumbh -= (over + rows - 1) / rows;
        if (thumbh < 56) thumbh = 56;
        cardh = thumbh + label_h + 16;
        panelh = title_h + rows * cardh + gap * (rows - 1) + pad;
    }
    if (panelw > aw - 20) panelw = aw - 20;
    int px = ax + (aw - panelw) / 2;
    int py = ay + (ah - panelh) / 2 + (int)((1.0f - t) * 10.0f);

    WMMotion::SoftShadow((float)px, (float)py, (float)panelw, (float)panelh, 18.0f,
                         24.0f, 10.0f, (P.dark ? 135 : 70) * fade / 255, false);
    MDraw::RoundRect(px, py, panelw, panelh, 18, P.panel, P.panel2,
                     (P.dark ? 242 : 250) * fade / 255);
    MDraw::RoundOutline(px, py, panelw, panelh, 18, P.line,
                        (P.dark ? 36 : 22) * fade / 255);

    uint32_t title_col = MDraw::Mix(P.panel, P.text, fade);
    uint32_t dim_col = MDraw::Mix(P.panel, P.text2, fade);
    FontTTF::DrawText(px + pad, py + 11, 16.0f, "Switch windows", title_col, true);
    char count[32];
    wm_tab_int_str(n, count);
    int cl = 0; while (count[cl]) cl++;
    count[cl++] = ' '; count[cl++] = 'w'; count[cl++] = 'i'; count[cl++] = 'n';
    count[cl++] = 'd'; count[cl++] = 'o'; count[cl++] = 'w'; count[cl++] = 's';
    count[cl] = 0;
    int cw = FontTTF::MeasureText(13.0f, count, false);
    FontTTF::DrawText(px + panelw - pad - cw, py + 14, 13.0f, count, dim_col, false);

    unsigned int since = now - wm_tab_sel_ms;
    float st = Animation::EaseMs(since, 110, Animation::EaseOutCubic);
    if (!wm_anim_on()) st = 1.0f;
    if (st < 0) st = 0;
    if (st > 1) st = 1;

    for (int k = 0; k < n; k++) {
        Window* win = GetWindow(ids[k]);
        if (!win) continue;
        int row = k / cols, col = k % cols;
        int cx = px + pad + col * (cardw + gap);
        int cy = py + title_h + row * (cardh + gap);
        bool selected = (k == sel_local);
        int sa = selected ? (int)(210.0f * st) : 0;
        uint32_t card_top = selected ? MDraw::Mix(P.panel2, accent, sa)
                                     : (P.dark ? 0xFF202128u : 0xFFE6E6EAu);
        uint32_t card_bot = selected ? MDraw::Mix(P.panel2, accent, sa / 2)
                                     : (P.dark ? 0xFF1B1C22u : 0xFFDFDFE5u);
        MDraw::RoundRect(cx, cy, cardw, cardh, 12, card_top, card_bot,
                         (P.dark ? 210 : 225) * fade / 255);
        if (selected)
            MDraw::RoundOutline(cx - 1, cy - 1, cardw + 2, cardh + 2, 13,
                                accent, 235 * fade / 255);

        int tx = cx + 8, ty = cy + 8;
        int tw = cardw - 16, th = thumbh;
        MDraw::RoundRect(tx, ty, tw, th, 8, 0xFF0B0C10u, 0xFF0B0C10u, fade);
        int slot = (int)(win - windows);
        WMTabThumb* bt = (slot >= 0 && slot < WM_MAX_WINDOWS) ? &wm_tab_thumbs[slot] : nullptr;
        if (bt && bt->valid && bt->px && bt->id == win->id && bt->w > 0 && bt->h > 0) {
            int dw = tw, dh = th;
            if ((long long)bt->w * th >= (long long)bt->h * tw) {
                dh = (int)(((long long)bt->h * tw) / bt->w);
                if (dh < 1) dh = 1;
            } else {
                dw = (int)(((long long)bt->w * th) / bt->h);
                if (dw < 1) dw = 1;
            }
            WMMotion::Snapshot sn;
            sn.px = bt->px; sn.w = bt->w; sn.h = bt->h;
            int dx = tx + (tw - dw) / 2, dy = ty + (th - dh) / 2;
            // a minimized window shows its last frame, dimmed. (satoru)
            int ba = win->state == WIN_MINIMIZED ? fade * 3 / 5 : fade;
            WMMotion::Blit(sn, 0, 0, bt->w, bt->h, dx, dy, dw, dh, ba);
        } else {
            int icon = win->app_icon >= 0 ? win->app_icon : AppIcons::IdForName(win->title);
            int isz = th / 2;
            if (isz < 24) isz = 24;
            if (isz > 56) isz = 56;
            if (isz > tw - 12) isz = tw - 12;
            MDraw::AppTile(icon, tx + (tw - isz) / 2, ty + (th - isz) / 2, isz);
        }

        char label[64];
        wm_tab_fit_text(win->title, 13.0f, cardw - 20, label, (int)sizeof(label));
        uint32_t lc = selected ? 0xFFFFFFFFu : MDraw::Mix(P.panel2, P.text, fade);
        FontTTF::DrawText(cx + 10, ty + th + 5, 13.0f, label, lc, false);
    }

    wm_damage(ax, ay, aw, ah);
    Graphics::MarkUIDirty();
}

// ── show desktop ────────────────────────────────────────────────────
void WindowManager::ToggleShowDesktop() {
    if (!wm_show_desktop) {
        unsigned long long mask[WM_MAX_WINDOWS / 64] = {0};
        int any = 0;
        wm_sd_focus = focused_id;
        for (int i = 0; i < WM_MAX_WINDOWS; i++) {
            Window* w = &windows[i];
            if (w->state == WIN_CLOSED || w->state == WIN_MINIMIZED || !w->visible) continue;
            mask[i / 64] |= (1ull << (i % 64));
            any++;
        }
        if (!any) return;
        for (int q = 0; q < WM_MAX_WINDOWS / 64; q++) wm_sd_mask[q] = mask[q];
        wm_show_desktop = true;
        for (int i = 0; i < WM_MAX_WINDOWS; i++) {
            if (mask[i / 64] & (1ull << (i % 64))) Minimize(windows[i].id);
        }
    } else {
        unsigned long long mask[WM_MAX_WINDOWS / 64];
        for (int q = 0; q < WM_MAX_WINDOWS / 64; q++) { mask[q] = wm_sd_mask[q]; wm_sd_mask[q] = 0; }
        wm_show_desktop = false;
        for (int i = 0; i < WM_MAX_WINDOWS; i++) {
            if (!(mask[i / 64] & (1ull << (i % 64)))) continue;
            Window* w = &windows[i];
            if (w->state == WIN_CLOSED) continue;
            if (w->state == WIN_MINIMIZED) Unminimize(w->id);
        }
        if (wm_sd_focus > 0 && GetWindow(wm_sd_focus)) Focus(wm_sd_focus);
        wm_sd_focus = -1;
    }
    Graphics::MarkUIDirty();
}

bool WindowManager::IsShowDesktop() {
    return wm_show_desktop;
}

void WindowManager::BringToFront(int id) {
    Window* win = GetWindow(id);
    if (!win) return;
    int max_z = -1;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (windows[i].state != WIN_CLOSED && windows[i].z_order > max_z)
            max_z = windows[i].z_order;
    }
    if (win->z_order == max_z) return;
    if (WM_UNLIKELY(max_z > 0x3FFFFFFF)) {
        // compress z-orders to avoid eventual overflow
        // (single linear pass; small N)
        for (int pass = 0; pass < WM_MAX_WINDOWS; pass++) {
            int min_z = 0x7FFFFFFF, min_i = -1;
            for (int i = 0; i < WM_MAX_WINDOWS; i++) {
                if (windows[i].state != WIN_CLOSED &&
                    windows[i].z_order >= pass && windows[i].z_order < min_z) {
                    min_z = windows[i].z_order; min_i = i;
                }
            }
            if (min_i < 0) break;
            windows[min_i].z_order = pass;
        }
        max_z = window_count;
    }
    win->z_order = max_z + 1;
    // z-order changed (no rect damage is raised here) - a cached drag backdrop
    // composed in the old order is now stale. (satoru)
    if (!wm_in_fast_render) InvalidateDragBackdrop();
}

void WindowManager::Focus(int id) {
    if (focused_id == id && id > 0) {
        Window* w = GetWindow(id);
        if (w && w->focused) { BringToFront(id); return; }
    }
    // unfocus old
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (windows[i].focused) {
            windows[i].focused = false;
            windows[i].dirty = true;
            // damage the whole footprint INCLUDING the shadow margin: losing focus
            // changes the titlebar tint, the accent line and the shadow, and the old
            // 1px edge strips missed the shadow band, leaving it stale behind a newly
            // focused overlapping window. focus changes are click-rare so the extra
            // repaint is free. (satoru)
            const int fm = WM_SHADOW_SIZE + 2;
            wm_damage(windows[i].x - fm, windows[i].y - fm,
                      windows[i].w + 2 * fm, windows[i].h + 2 * fm);
        }
    }
    Window* win = GetWindow(id);
    if (win) {
        win->focused = true;
        win->dirty = true;
        wm_damage_window(win);
        focused_id = id;
        BringToFront(id);
        // a wayland window's client gets the keyboard with the wm focus (dock,
        // alt-tab, titlebar click), not only on a click in its content. (satoru)
        WaylandServer::NotifyWmFocus(id);
        if (Keyboard::GetScreenReader()) {
            Keyboard::Announce(win->title);
        }
    } else {
        focused_id = -1;
    }
}

void WindowManager::SetTitle(int id, const char* title) {
    Window* win = GetWindow(id);
    if (!win) return;
    wmcpy(win->title, title, 64);
    win->dirty = true;
    wm_damage(win->x, win->y, win->w, WM_TITLEBAR_H);
    // a background window's titlebar text changed - invalidate the snapshot
    // so it isn't shown with the old title behind a drag. (satoru)
    if (!wm_in_fast_render) InvalidateDragBackdrop();
}

// a client reported a stable app id: switch the geometry key from the initial
// title to the icon id. before the first frame this also applies a remembered
// rect, so firefox and other wayland apps reopen where they were left. (satoru)
void WindowManager::SetAppIcon(int id, int icon) {
    Window* win = GetWindow(id);
    if (!win) return;
    win->app_icon = icon;
    int slot = (int)(win - windows);
    if (icon < 0 || slot < 0 || slot >= WM_MAX_WINDOWS) return;
    char key[WM_GEOM_KEY];
    wm_geom_key_icon(icon, key, WM_GEOM_KEY);
    if (wm_geom_streq(key, wm_win_key[slot])) return;
    int n = 0;
    while (key[n] && n < WM_GEOM_KEY - 1) { wm_win_key[slot][n] = key[n]; n++; }
    wm_win_key[slot][n] = 0;
    if (!win->had_last) {
        if (!ApplySavedGeometry(win, key))
            wm_geom_store(key, win->x, win->y, win->w, win->h);
    }
}

void WindowManager::MoveWindow(int id, int x, int y) {
    Window* win = GetWindow(id);
    if (!win) return;
    if (win->x == x && win->y == y) return;
    wm_damage_window(win);
    win->x = x; win->y = y;
    win->dirty = true;
    UpdateContentArea(win);
    UpdateWindowMonitor(win);   // window may have crossed a monitor edge (satoru)
    wm_damage(win->x, win->y, win->w, win->h);
}

void WindowManager::ResizeWindow(int id, int w, int h) {
    Window* win = GetWindow(id);
    if (!win) return;
    if (w < WM_MIN_WIDTH) w = WM_MIN_WIDTH;
    if (h < WM_MIN_HEIGHT) h = WM_MIN_HEIGHT;
    if (win->w == w && win->h == h) return;
    wm_damage_window(win);
    win->w = w; win->h = h;
    win->dirty = true;
    UpdateContentArea(win);
    wm_damage(win->x, win->y, win->w, win->h);
}

void WindowManager::SetVisible(int id, bool visible) {
    Window* win = GetWindow(id);
    if (!win) return;
    if (win->visible == visible) return;
    wm_damage_window(win);
    win->visible = visible;
    win->dirty = true;
    if (!visible) win->had_last = false;
}

void WindowManager::CollectLastPaintedDamage() {
    const int m = WM_SHADOW_SIZE + 2;
    const int xb = Graphics::ScissorEnabled() ? 16 : 0;   // the shadow's offset and blur, see wm_damage_window (satoru)
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        Window* w = &windows[i];
        if (!w->had_last && w->fx_bx1 <= w->fx_bx0) continue;
        bool moved = w->had_last && (w->last_x != w->x || w->last_y != w->y ||
                                     w->last_w != w->w || w->last_h != w->h);
        if (moved) {
            if (w->id == action_window_id && current_action == WM_DRAG) {
                wm_fp_damage(w->last_x - m, w->last_y - m, w->last_w + 2*m, w->last_h + 2*m + xb);
                wm_fp_damage(w->x - m, w->y - m, w->w + 2*m, w->h + 2*m + xb);
            } else {
                wm_damage(w->last_x - m, w->last_y - m, w->last_w + 2*m, w->last_h + 2*m + xb);
                wm_damage(w->x - m, w->y - m, w->w + 2*m, w->h + 2*m + xb);
            }
        }
        // spring motion paints at the eased rect, not the real geometry, so
        // damage what it actually painted last frame while it is still moving. (satoru)
        if (w->fx_mode) {
            if (w->fx_bx1 > w->fx_bx0 && w->fx_by1 > w->fx_by0)
                wm_damage(w->fx_bx0 - m, w->fx_by0 - m,
                          w->fx_bx1 - w->fx_bx0 + 2*m, w->fx_by1 - w->fx_by0 + 2*m + xb);
        }
    }
}

// ── move by copy (kurono.gfx.scissor=1) ─────────────────────────────
// a dragged window's pixels do not change when it moves: the back buffer
// already holds them at the old place (under the damage scissor the back
// buffer outside the damage is exactly what was presented). so instead of
// repainting the old and the new footprint from the wallpaper up (the whole
// window and everything under it, every frame) they are moved with one
// memmove, and only what the move uncovered, the rounded corners (blended
// with what is behind) and the titlebar (hover) are repainted. any other
// damage marked this frame, app content included, is kept and repainted on
// top as usual. it only runs when nothing but the pointer can be drawn over
// the window: it is the top window, opaque, settled, inside the screen, clear
// of the menu bar and the dock, with no snap preview, menu, switcher, control
// center or toast up. the new rect is reported back so it is marked for the
// present after the passes. (satoru)
static bool wm_copy_ok(const Window* win, int ex0, int ey0, int ex1, int ey1) {
    if (!win || win->state == WIN_CLOSED || !win->visible || win->state == WIN_MINIMIZED) return false;
    if (win->state == WIN_FULLSCREEN || win->anim_kind != 0 || win->fx_mode != 0 || win->alpha < 255) return false;
    if (comp_shadow_during_drag || wm_sp_edge >= 0 || wm_tab_open || WindowManager::IsContextMenuOpen()) return false;
    if (ControlCenter::IsOpen() || ControlCenter::IsAnimating() || NotificationManager::ActiveCount() > 0) return false;
    if (Taskbar::IsAnimating() || Taskbar::volume_popup_open || Taskbar::StartMenuOpen()) return false;
    // the back buffer has to hold this window on top: it was the top window
    // when the last frame was drawn (a click that raises it marks the raise,
    // and a move in the same frame would otherwise take that repaint back). (satoru)
    if (win->id != wm_last_top_id) return false;
    const int sw = WindowManager::GetScreenWidth(), sh = WindowManager::GetScreenHeight();
    if (win->last_x < 0 || win->last_y < 0 || win->last_x + win->last_w > sw || win->last_y + win->last_h > sh) return false;
    // the menu bar and the dock (and its tooltips) draw over windows. (satoru)
    if (ey0 < Taskbar::TopInset() + 2 && ex0 < Taskbar::screen_width) return false;
    if (ey1 > Taskbar::y_pos - 72 && ex0 < Taskbar::screen_width) return false;
    const Window* ws = WindowManager::GetWindows();
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        const Window* o = &ws[i];
        if (o == win || o->state == WIN_CLOSED || !o->visible || o->state == WIN_MINIMIZED) continue;
        if (o->z_order <= win->z_order) continue;
        if (o->x < ex1 && o->x + o->w > ex0 && o->y < ey1 && o->y + o->h > ey0) return false;
    }
    return true;
}

bool WindowManager::DragMoveByCopy(int* rx, int* ry, int* rw, int* rh) {
    int n = wm_fp_n;
    bool over = wm_fp_overflow;
    wm_fp_n = 0;
    wm_fp_overflow = false;
    if (n == 0 || over || !IsWindowDragActive()) return false;
    Window* win = GetWindow(action_window_id);
    if (!win || !win->had_last) return false;
    if (win->last_w != win->w || win->last_h != win->h) return false;
    int dx = win->x - win->last_x, dy = win->y - win->last_y;
    if (dx == 0 && dy == 0) return false;
    // everything the footprints covered. (satoru)
    int ex0 = wm_fp_rects[0].x, ey0 = wm_fp_rects[0].y;
    int ex1 = ex0 + wm_fp_rects[0].w, ey1 = ey0 + wm_fp_rects[0].h;
    for (int i = 1; i < n; i++) {
        const Graphics::Rect& r = wm_fp_rects[i];
        if (r.x < ex0) ex0 = r.x;
        if (r.y < ey0) ey0 = r.y;
        if (r.x + r.w > ex1) ex1 = r.x + r.w;
        if (r.y + r.h > ey1) ey1 = r.y + r.h;
    }
    const int sw = GetScreenWidth(), sh = GetScreenHeight();
    if (ex0 < 0) ex0 = 0;
    if (ey0 < 0) ey0 = 0;
    if (ex1 > sw) ex1 = sw;
    if (ey1 > sh) ey1 = sh;
    if (!wm_copy_ok(win, ex0, ey0, ex1, ey1)) return false;
    if (!Graphics::RemoveFrameDamage(wm_fp_rects, n)) return false;
    // the window's pixels, clipped to the screen at the new place. (satoru)
    Graphics::MoveRect(win->last_x, win->last_y, win->w, win->h, dx, dy);
    const int nx = win->x, ny = win->y, nw = win->w, nh = win->h;
    // what the move uncovered: the footprint box minus the new rect, as four
    // bands. (satoru)
    wm_damage(ex0, ey0, ex1 - ex0, ny - ey0);
    wm_damage(ex0, ny + nh, ex1 - ex0, ey1 - (ny + nh));
    wm_damage(ex0, ny, nx - ex0, nh);
    wm_damage(nx + nw, ny, ex1 - (nx + nw), nh);
    // corners blend with what is behind, the titlebar has hover states. (satoru)
    int r = wm_radius(win) + 2;
    wm_damage(nx, ny, nw, WM_TITLEBAR_H > r ? WM_TITLEBAR_H : r);
    wm_damage(nx, ny + nh - r, r, r);
    wm_damage(nx + nw - r, ny + nh - r, r, r);
    // a software arrow drawn over the old place moved with the pixels. (satoru)
    int cx, cy;
    if (Mouse::LastSoftwarePos(cx, cy)) Mouse::DamageCursor(cx + dx, cy + dy);
    *rx = nx; *ry = ny; *rw = nw; *rh = nh;
    return true;
}

void WindowManager::MarkDirty(int id) {
    Window* win = GetWindow(id);
    if (!win) return;
    win->dirty = true;
    wm_damage(win->x, win->y, win->w, win->h);
    // only a change to a window OTHER than the one being dragged makes the
    // backdrop stale - the dragged window's content is redrawn every fast
    // frame anyway, so its own MarkDirty must not drop the snapshot. (satoru)
    if (!wm_in_fast_render && !(IsWindowDragActive() && id == action_window_id)) {
        InvalidateDragBackdrop();
    }
}

int WindowManager::GetWindowCount() {
    int count = 0;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (windows[i].state != WIN_CLOSED) count++;
    }
    return count;
}

Window* WindowManager::GetWindows() { return windows; }
int WindowManager::GetScreenWidth() { return screen_width; }
int WindowManager::GetScreenHeight() { return screen_height; }
int WindowManager::GetDesktopY() { return wm_desktop_y; }
int WindowManager::GetDesktopH() { return wm_desktop_h; }

int WindowManager::GetFocusedIndex() { return focused_id; }

void WindowManager::SetDesktopArea(int x, int y, int w, int h) {
    wm_desktop_x = x;
    wm_desktop_y = y;
    wm_desktop_w = w;
    wm_desktop_h = h;
    if (wm_desktop_w < WM_MIN_WIDTH) wm_desktop_w = WM_MIN_WIDTH;
    if (wm_desktop_h < WM_MIN_HEIGHT) wm_desktop_h = WM_MIN_HEIGHT;
    int total_w = x + w;
    int total_h = y + h;
    if (total_w > screen_width) screen_width = total_w;
    if (total_h > screen_height) screen_height = total_h;
}

bool WindowManager::IsPointInWindow(Window* win, int px, int py) {
    if (!win || !win->visible || win->state == WIN_MINIMIZED) return false;
    // a window on its way out (closing, draining into the dock) takes no input (satoru)
    if (win->fx_mode == FX_CLOSE || win->fx_mode == FX_MIN) return false;
    return px >= win->x && px < win->x + win->w &&
           py >= win->y && py < win->y + win->h;
}

void WindowManager::UpdateContentArea(Window* win) {
    if (win->has_titlebar) {
        win->content_x = win->x + WM_BORDER_W;
        win->content_y = win->y + WM_TITLEBAR_H;
        // clamp: a window resized below chrome size would make these negative, which
        // the unsigned clip math then reads as a huge extent. (satoru)
        win->content_w = win->w > WM_BORDER_W * 2 ? win->w - WM_BORDER_W * 2 : 0;
        win->content_h = win->h > WM_TITLEBAR_H + WM_BORDER_W
                       ? win->h - WM_TITLEBAR_H - WM_BORDER_W : 0;
    } else {
        win->content_x = win->x;
        win->content_y = win->y;
        win->content_w = win->w;
        win->content_h = win->h;
    }
}

WMAction WindowManager::HitTest(Window* win, int px, int py) {
    if (!win || !win->visible || win->state == WIN_MINIMIZED) return WM_NONE;
    const int r = 6; // resize border sensitivity

    if (win->resizable && win->state != WIN_MAXIMIZED) {
        bool top    = (py >= win->y && py < win->y + r);
        bool bottom = (py >= win->y + win->h - r && py < win->y + win->h);
        bool left   = (px >= win->x && px < win->x + r);
        bool right  = (px >= win->x + win->w - r && px < win->x + win->w);

        if (top && left)   return WM_RESIZE_TL;
        if (top && right)  return WM_RESIZE_TR;
        if (bottom && left)  return WM_RESIZE_BL;
        if (bottom && right) return WM_RESIZE_BR;
        if (top)    return WM_RESIZE_T;
        if (bottom) return WM_RESIZE_B;
        if (left)   return WM_RESIZE_L;
        if (right)  return WM_RESIZE_R;
    }

    // titlebar drag - skipped for fullscreen since drag has no effect
    if (win->has_titlebar && win->state != WIN_FULLSCREEN &&
        py >= win->y && py < win->y + WM_TITLEBAR_H &&
        px >= win->x && px < win->x + win->w) {
        return WM_DRAG;
    }

    return WM_NONE;
}

int WindowManager::TopWindowAt(int px, int py) {
    int best_id = -1, best_z = -1;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (windows[i].state != WIN_CLOSED && windows[i].visible &&
            windows[i].state != WIN_MINIMIZED) {
            if (IsPointInWindow(&windows[i], px, py) && windows[i].z_order > best_z) {
                best_z = windows[i].z_order;
                best_id = windows[i].id;
            }
        }
    }
    return best_id;
}

void WindowManager::SortByZOrder() {
    // insertion sort - N <= 32, almost-sorted in practice
    for (int i = 1; i < WM_MAX_WINDOWS; i++) {
        Window key = windows[i];
        int j = i - 1;
        while (j >= 0 && windows[j].z_order > key.z_order) {
            windows[j+1] = windows[j];
            j--;
        }
        windows[j+1] = key;
    }
}

// Apply a drag/resize action.  Centralised so Update() and HandleMouseMove()
// stay in sync and all eight resize handles work.  Returns true if any
// geometry changed.
static bool wm_apply_action(Window* win, WMAction act, int mx, int my,
                            int dx_off, int dy_off) {
    if (!win) return false;
    int ox = win->x, oy = win->y, ow = win->w, oh = win->h;
    switch (act) {
        case WM_DRAG:
            win->x = mx - dx_off;
            win->y = my - dy_off;
            wm_clamp_drag_bounds(win);
            break;
        case WM_RESIZE_BR:
            win->w = mx - win->x;
            win->h = my - win->y;
            wm_clamp_resize_bounds(win);
            break;
        case WM_RESIZE_R:
            win->w = mx - win->x;
            wm_clamp_resize_bounds(win);
            break;
        case WM_RESIZE_B:
            win->h = my - win->y;
            wm_clamp_resize_bounds(win);
            break;
        case WM_RESIZE_L: {
            int new_x = mx;
            int new_w = (win->x + win->w) - new_x;
            if (new_w >= WM_MIN_WIDTH) { win->x = new_x; win->w = new_w; }
            wm_clamp_drag_bounds(win);
            wm_clamp_resize_bounds(win);
            break;
        }
        case WM_RESIZE_T: {
            int new_y = my;
            int new_h = (win->y + win->h) - new_y;
            if (new_h >= WM_MIN_HEIGHT) { win->y = new_y; win->h = new_h; }
            wm_clamp_drag_bounds(win);
            wm_clamp_resize_bounds(win);
            break;
        }
        case WM_RESIZE_BL: {
            int new_x = mx;
            int new_w = (win->x + win->w) - new_x;
            if (new_w >= WM_MIN_WIDTH) { win->x = new_x; win->w = new_w; }
            win->h = my - win->y;
            wm_clamp_drag_bounds(win);
            wm_clamp_resize_bounds(win);
            break;
        }
        case WM_RESIZE_TR: {
            int new_y = my;
            int new_h = (win->y + win->h) - new_y;
            if (new_h >= WM_MIN_HEIGHT) { win->y = new_y; win->h = new_h; }
            win->w = mx - win->x;
            wm_clamp_drag_bounds(win);
            wm_clamp_resize_bounds(win);
            break;
        }
        case WM_RESIZE_TL: {
            int new_x = mx;
            int new_w = (win->x + win->w) - new_x;
            if (new_w >= WM_MIN_WIDTH) { win->x = new_x; win->w = new_w; }
            int new_y = my;
            int new_h = (win->y + win->h) - new_y;
            if (new_h >= WM_MIN_HEIGHT) { win->y = new_y; win->h = new_h; }
            wm_clamp_drag_bounds(win);
            wm_clamp_resize_bounds(win);
            break;
        }
        default: return false;
    }
    if (win->x == ox && win->y == oy && win->w == ow && win->h == oh) return false;
    UiDiag::NoteWindowMove();
    return true;
}

void WindowManager::Update(int mouse_x, int mouse_y, bool mouse_down, bool mouse_clicked) {
    // handle ongoing drag/resize
    if (current_action != WM_NONE && mouse_down) {
        Window* win = GetWindow(action_window_id);
        if (WM_LIKELY(win != nullptr)) {
            if (current_action == WM_DRAG) wm_snap_preview_update(win, current_action, mouse_x, mouse_y);
            unsigned int now = Timer::GetRealMs();
            bool throttle = (current_action == WM_DRAG) && wm_drag_min_gap_ms > 0
                            && (now - wm_last_drag_ms) < wm_drag_min_gap_ms;
            if (!throttle) {
                int ox = win->x, oy = win->y, ow = win->w, oh = win->h;
                if (wm_apply_action(win, current_action, mouse_x, mouse_y,
                                    drag_offset_x, drag_offset_y)) {
                    wm_fp_damage(ox - WM_SHADOW_SIZE, oy - WM_SHADOW_SIZE,
                                 ow + 2*WM_SHADOW_SIZE, oh + 2*WM_SHADOW_SIZE);
                    wm_fp_damage(win->x - WM_SHADOW_SIZE, win->y - WM_SHADOW_SIZE,
                                 win->w + 2*WM_SHADOW_SIZE, win->h + 2*WM_SHADOW_SIZE);
                    UpdateContentArea(win);
                    win->dirty = true;
                }
                wm_last_drag_ms = now;
            }
        } else {
            current_action = WM_NONE;
            action_window_id = -1;
        }
    }

    if (!mouse_down) {
        if (current_action != WM_NONE) {
            Window* w = GetWindow(action_window_id);
            if (w) wm_geom_remember(w, true);
            InvalidateDragBackdrop(); // drag ended (satoru)
        }
        wm_snap_preview_clear();
        current_action = WM_NONE;
        action_window_id = -1;
    }

    // handle new clicks
    if (mouse_clicked) {
        int top_id = TopWindowAt(mouse_x, mouse_y);
        if (top_id > 0) {
            Focus(top_id);
            Window* win = GetWindow(top_id);
            if (win) {
                // macos traffic light buttons on left side
                int btn_cy = win->y + WM_TITLEBAR_H / 2;
                int btn_start_x = win->x + 14;
                const int btn_r2 = 8 * 8; // click radius squared

                // close button. stamp the press time first so the traffic-light
                // press-dip eases even on the frame the window starts closing. (satoru)
                if (win->closable) {
                    int dx = mouse_x - btn_start_x;
                    int dy = mouse_y - btn_cy;
                    if (dx*dx + dy*dy <= btn_r2) {
                        win->tb_press_ms = Timer::GetRealMs();
                        UserCloseWindow(top_id);
                        return;
                    }
                }

                // minimize button
                {
                    int dx = mouse_x - (btn_start_x + 22);
                    int dy = mouse_y - btn_cy;
                    if (dx*dx + dy*dy <= btn_r2) {
                        win->tb_press_ms = Timer::GetRealMs();
                        Minimize(top_id);
                        return;
                    }
                }

                // maximize button
                {
                    int dx = mouse_x - (btn_start_x + 44);
                    int dy = mouse_y - btn_cy;
                    if (dx*dx + dy*dy <= btn_r2) {
                        win->tb_press_ms = Timer::GetRealMs();
                        ToggleMaximize(top_id);
                        return;
                    }
                }

                // start drag/resize
                WMAction action = HitTest(win, mouse_x, mouse_y);
                if (action != WM_NONE) {
                    current_action = action;
                    action_window_id = top_id;
                    drag_offset_x = mouse_x - win->x;
                    drag_offset_y = mouse_y - win->y;
                    wm_last_drag_ms = Timer::GetRealMs();
                }

                // pass click to window input handler
                if (win->input && action == WM_NONE &&
                    mouse_x >= win->content_x &&
                    mouse_x <  win->content_x + win->content_w &&
                    mouse_y >= win->content_y &&
                    mouse_y <  win->content_y + win->content_h) {
                    int local_x = mouse_x - win->content_x;
                    int local_y = mouse_y - win->content_y;
                    { TaskMem::WindowScope _tms(win->id); win->input(win, 1, local_x, local_y); } // event 1 = click
                }
            }
        }
    }

    prev_mouse_down = mouse_down;
}

void WindowManager::RenderShadow(Window* win) {
    if (!comp_shadow_enabled) return;
    if (win->state == WIN_FULLSCREEN) return;
    if (UIStyle::Modern()) {
        // layered soft shadow: a wide ambient shadow plus a tight contact one.
        // focus raises the window (deeper, longer shadow), eased by the
        // elevation spring. (satoru)
        float e = win->fx_el.x;
        if (e < 0.0f) e = 0.0f;
        if (e > 1.0f) e = 1.0f;
        int op = comp_shadow_opacity_pct;
        float k = (float)op / 60.0f;
        float rad = (float)wm_radius(win);
        float fx = (float)win->x, fy = (float)win->y, fw = (float)win->w, fh = (float)win->h;
        WMMotion::SoftShadow(fx, fy, fw, fh, rad + 6.0f, 22.0f + 16.0f * e, 8.0f + 8.0f * e,
                             (int)((70.0f + 40.0f * e) * k), true);
        WMMotion::SoftShadow(fx, fy, fw, fh, rad, 3.0f + 2.0f * e, 1.0f + 1.0f * e,
                             (int)((60.0f + 20.0f * e) * k), true);
        return;
    }
    if (win->state == WIN_MAXIMIZED) return;
    int sz = comp_shadow_radius;
    if (win->focused) sz += 3;
    if (sz < 1) return;
    if (sz > 16) sz = 16;
    int base = win->focused ? 20 : 12;
    int scaled_base = (base * comp_shadow_opacity_pct) / 100;
    if (scaled_base <= 0) return;
    int wx = win->x, wy = win->y, ww = win->w, wh = win->h;
    int wr = wx + ww;
    int wb = wy + wh;
    for (int i = 1; i <= sz; i++) {
        unsigned int alpha = (unsigned int)(scaled_base * (sz - i + 1));
        if (alpha > 0xFF) alpha = 0xFF;
        // bottom shadow (wider)
        if (ww - i > 0)
            Graphics::FillRectAlpha(wx + i, wb + i, ww - i, 1, (uint8_t)alpha, 0xFF000000u);
        // right shadow
        if (wh - 4 > 0)
            Graphics::FillRectAlpha(wr + i, wy + i + 4, 1, wh - 4, (uint8_t)alpha, 0xFF000000u);
        // left shadow (subtle)
        if (i <= 3 && wh - 4 > 0)
            Graphics::FillRectAlpha(wx - i, wy + i + 4, 1, wh - 4, (uint8_t)(alpha >> 1), 0xFF000000u);
    }
}

// true when a window body colour is light enough to need dark chrome on top. (satoru)
static bool wm_is_light(uint32_t c) {
    uint32_t l = (((c >> 16) & 0xFF) * 299 + ((c >> 8) & 0xFF) * 587 + (c & 0xFF) * 114) / 1000;
    return l > 150;
}

void WindowManager::RenderTitlebar(Window* win) {
    bool focused = win->focused;
    // ease the titlebar color across focus changes instead of snapping - the
    // tween engine keeps its own state per window; damage the strip while the
    // ease runs so the partial-present path keeps painting it. (satoru)
    uint32_t bg_target = focused ? COL_TITLE_FOCUSED : COL_TITLE_BG;
    uint32_t bg = KSS::Anim::Color(KSS::Motion::Id((uint32_t)win->id, 0x31u),
                                   bg_target, KSS::Motion::Micro, KSS::Motion::Std);
    if (bg != bg_target)
        wm_damage(win->x, win->y, win->w, WM_TITLEBAR_H + 1);
    uint32_t text_col = focused ? COL_TITLE_TEXT    : COL_TITLE_TEXT_DIM;
    int wx = win->x, wy = win->y, ww = win->w;
    // a light window body (settings in the light appearance) gets dark title
    // text and a dark hairline so the unified titlebar stays readable. (satoru)
    const bool light_body = wm_is_light(win->bg_color);
    if (light_body) text_col = focused ? 0xFF1C1C1E : 0xFF8A8A92;

    // modern: a unified titlebar - same surface as the body (already drawn by
    // RenderWindowBody), set off by a hairline instead of a darker strip.
    // (satoru)
    const bool modern = UIStyle::Modern();
    if (!modern) {
        // titlebar background with rounded top corners
        Graphics::FillRoundedRect(wx, wy, ww, WM_TITLEBAR_H, WM_CORNER_RADIUS, bg);
        Graphics::FillRect(wx, wy + WM_CORNER_RADIUS, ww, WM_TITLEBAR_H - WM_CORNER_RADIUS, bg);
    }

    if (focused && !modern) {
        for (int row = 0; row < 4; row++) {
            uint8_t a = (uint8_t)(30 - row * 6);
            if (ww > 2)
                Graphics::FillRectAlpha(wx + 1, wy + WM_TITLEBAR_H - 4 + row,
                                        ww - 2, 1, a, 0xFF000000u);
        }
    }

    if (modern) Graphics::FillRectAlpha(wx + 1, wy + WM_TITLEBAR_H, ww - 2, 1, light_body ? 16 : 22,
                                        light_body ? 0xFF000000u : 0xFFFFFFFFu);
    else        Graphics::FillRect(wx, wy + WM_TITLEBAR_H, ww, 1, COL_SEPARATOR);

    int btn_y = wy + WM_TITLEBAR_H / 2;
    const int btn_r = 6;
    int btn_start_x = wx + 14;

    // ── traffic-light buttons with eased hover-grow + press-dip ──────────────
    // each button's fill brightens toward white and its radius grows ~2px on
    // hover, and dips ~1px for ~120ms after a press - all eased through the kss
    // anim engine keyed by (window id, button index), so motion is shared with
    // the rest of the desktop and costs only table-driven float lerps. (satoru)
    {
        const int cx[3] = { btn_start_x, btn_start_x + 22, btn_start_x + 44 };
        const uint32_t base[3] = { COL_CLOSE_BTN, COL_MIN_BTN, COL_MAX_BTN };
        unsigned int pe = Timer::GetRealMs() - win->tb_press_ms;
        bool pressing = win->tb_press_ms != 0 && pe < KSS::Motion::Micro;
        bool moving = pressing;   // does the cluster still need to repaint? (satoru)
        for (int i = 0; i < 3; i++) {
            if (i == 0 && !win->closable) continue;
            bool hov = (win->tb_hover_btn == i);
            // hover lift 0..1 and a fill brightened toward white when hovered. (satoru)
            float lift = KSS::Anim::Float(KSS::Motion::Id((uint32_t)win->id, 0x10u + i),
                                          hov ? 1.0f : 0.0f, KSS::Motion::Micro, KSS::Motion::Std);
            uint32_t fill = KSS::Anim::Color(KSS::Motion::Id((uint32_t)win->id, 0x20u + i),
                                             hov ? Animation::LerpColor(base[i], 0xFFFFFFFF, 70)
                                                 : base[i], KSS::Motion::Micro, KSS::Motion::Std);
            int r = btn_r + (int)(lift * 2.0f + 0.5f);
            if (pressing && hov) r -= 1;            // tactile press-dip (satoru)
            if (modern) {
                // unfocused windows show quiet grey lights; pointing at the
                // cluster brings the colour back (as on macos) (satoru)
                bool cluster = win->tb_hover_btn >= 0;
                if (!focused && !cluster) fill = light_body ? 0xFFD0D0D6 : 0xFF4A4B55;
                MDraw::Circle(cx[i], btn_y, modern ? btn_r : r, fill, 255);
                if (pressing && hov) MDraw::Circle(cx[i], btn_y, btn_r, 0xFF000000u, 50);
                (void)lift;
            } else {
                Graphics::FillCircle(cx[i], btn_y, r, fill);
            }
            // a button is still in motion until its lift settles to fully on/off;
            // that's the cheap per-frame "is the hover ease running" signal. (satoru)
            if ((hov && lift < 0.999f) || (!hov && lift > 0.001f)) moving = true;
        }
        // damage ONLY the traffic-light cluster while it eases so the present
        // stays partial (the whole titlebar/screen is not re-copied for a 140ms
        // micro-interaction). cluster spans btn_start_x-8 .. +44+8, height = the
        // grown circle. the hover state-change itself already raised MarkUIDirty
        // in UpdateTitlebarHover; this keeps the render gate open AND scopes the
        // framebuffer copy for the in-between frames. (satoru)
        if (moving)
            wm_damage(btn_start_x - 8, btn_y - (btn_r + 3),
                      (44 + 16), (btn_r + 3) * 2 + 2);
    }

    if (modern) {
        // glyphs only while the pointer is on the cluster: x, minus, and the
        // two full-screen wedges, anti-aliased dark ink (satoru)
        if (win->tb_hover_btn >= 0) {
            const uint32_t gi = 0xFF4A1A14, gm = 0xFF5A3A08, gx = 0xFF0E4A16;
            int c0 = btn_start_x, c1 = btn_start_x + 22, c2 = btn_start_x + 44;
            if (win->closable)
                for (int k = -3; k <= 3; k++) {
                    MDraw::RoundRect(c0 + k, btn_y + k, 1, 1, 0, gi, 230);
                    MDraw::RoundRect(c0 + k, btn_y - k, 1, 1, 0, gi, 230);
                    MDraw::RoundRect(c0 + k + 1, btn_y + k, 1, 1, 0, gi, 90);
                    MDraw::RoundRect(c0 + k + 1, btn_y - k, 1, 1, 0, gi, 90);
                }
            MDraw::RoundRect(c1 - 4, btn_y - 1, 8, 2, 1, gm, 230);
            for (int k = 0; k < 4; k++) {
                MDraw::RoundRect(c2 - 3, btn_y - 3 + k, 4 - k, 1, 0, gx, 230);   // upper left wedge (satoru)
                MDraw::RoundRect(c2 + k, btn_y + 3 - k, 4 - k, 1, 0, gx, 230);   // lower right wedge (satoru)
            }
        }
    } else if (win->closable && focused) {
        Graphics::DrawLine(btn_start_x - 2, btn_y - 2, btn_start_x + 2, btn_y + 2, 0xFF401010);
        Graphics::DrawLine(btn_start_x + 2, btn_y - 2, btn_start_x - 2, btn_y + 2, 0xFF401010);
    }
    if (focused && !modern) {
        Graphics::FillRect(btn_start_x + 19, btn_y, 6, 1, 0xFF403010);
        Graphics::DrawLine(btn_start_x + 42, btn_y - 2, btn_start_x + 46, btn_y + 2, 0xFF103010);
        Graphics::DrawLine(btn_start_x + 46, btn_y - 2, btn_start_x + 42, btn_y + 2, 0xFF103010);
    }

    int title_area_start = btn_start_x + 62;
    int title_area_end = wx + ww - 10;
    if (title_area_end > title_area_start + 8) {
        int max_chars = (title_area_end - title_area_start) / 8;
        int len = wmlen(win->title);
        char tbuf[128];
        const char* draw = win->title;
        if (len > max_chars && max_chars >= 4 && max_chars < (int)sizeof(tbuf)) {
            // truncate with a trailing ellipsis so a cut-off title is visibly marked
            // instead of being silently clipped mid-glyph. (satoru)
            int keep = max_chars - 3;
            for (int c = 0; c < keep; c++) tbuf[c] = win->title[c];
            tbuf[keep] = '.'; tbuf[keep + 1] = '.'; tbuf[keep + 2] = '.'; tbuf[keep + 3] = 0;
            draw = tbuf; len = max_chars;
        } else if (len > max_chars) {
            len = max_chars;
        }
        // modern: semibold title centred on the whole window (as on macos),
        // measured with the real font; classic keeps the 8px grid. (satoru)
        int title_w = modern ? FontTTF::MeasureText(15.0f, draw, true) : len * 8;
        int title_x = modern ? wx + (ww - title_w) / 2
                             : title_area_start + (title_area_end - title_area_start - title_w) / 2;
        if (title_x < title_area_start) title_x = title_area_start;
        Graphics::PushClipRect(title_area_start, wy,
                              title_area_end - title_area_start, WM_TITLEBAR_H);
        // app logo just left of the title: pinned id first (wayland set_app_id),
        // else derived from the title text; skip the generic placeholder so
        // plain windows stay clean. (satoru)
        int gid = (win->app_icon >= 0) ? win->app_icon
                                       : AppIcons::IdForName(win->title);
        if (gid != AppIcons::GENERIC && title_x - 22 >= title_area_start) {
            AppIcons::Draw(gid, title_x - 22, wy + (WM_TITLEBAR_H - 16) / 2, 16);
        }
        if (modern) FontTTF::DrawText(title_x, wy + (WM_TITLEBAR_H - 18) / 2, 15.0f, draw, text_col, true);
        else        Graphics::DrawString(title_x, wy + (WM_TITLEBAR_H - 12) / 2, draw, text_col, 0x00000000);
        Graphics::PopClipRect();
    }

    if (focused && !modern && ww > 2 * WM_CORNER_RADIUS) {
        Graphics::FillRect(wx + WM_CORNER_RADIUS, wy, ww - 2 * WM_CORNER_RADIUS, 1, wm_get_accent());
    }
}

// Compute the animation phase (0..256, fixed-point Q8) for a window.
// Returns 256 (= 1.0) if no animation is active or animation has
// completed.  Used by Render() to drive the per-window post-pass.
// ── window motion (designs: "Kurono motion drafts") ─────────────────
// all times below are design-milliseconds at the default 180 ms animation
// speed setting; compositor.animation_speed_ms scales them proportionally.
// (satoru)
static int wm_speed_q8() {
    int s = (comp_anim_duration_ms * 256) / 180;
    if (s < 64) s = 64;
    if (s > 1024) s = 1024;
    return s;
}

static unsigned short wm_motion_dur(int design_ms) {
    return (unsigned short)((design_ms * wm_speed_q8()) / 256);
}

// elapsed design-ms since the window's animation started.
static int wm_motion_t(const Window* win, unsigned int now) {
    return (int)(((long long)(now - win->anim_start_ms) * 256) / wm_speed_q8());
}

static WMMotion::Snapshot wm_snap(const Window* win) {
    WMMotion::Snapshot s;
    s.px = win->snap_px; s.w = win->snap_w; s.h = win->snap_h;
    return s;
}

static void wm_snap_release(Window* win) {
    WMMotion::Snapshot s = wm_snap(win);
    WMMotion::Release(s);
    win->snap_px = nullptr; win->snap_w = win->snap_h = 0;
}

// capture the window as just drawn into the frame being composed.
static void wm_snap_capture(Window* win) {
    WMMotion::Snapshot s = wm_snap(win);
    WMMotion::Capture(s, win->x, win->y, win->w, win->h, WM_CORNER_RADIUS);
    win->snap_px = s.px; win->snap_w = s.w; win->snap_h = s.h;
}

// the window's taskbar tile centre + size; falls back to the bottom edge under
// the window if it has no button showing.
static void wm_tile_target(const Window* win, int scr_h, int& cx, int& cy, int& tw, int& th) {
    int tx, ty;
    if (Taskbar::WindowTileRect(win->id, tx, ty, tw, th)) {
        cx = tx + tw / 2; cy = ty + th / 2;
    } else {
        tw = 28; th = 24;
        cx = win->x + win->w / 2; cy = scr_h - 22;
    }
}

static void wm_damage_union(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) {
    int x0 = ax < bx ? ax : bx, y0 = ay < by ? ay : by;
    int x1 = ax + aw > bx + bw ? ax + aw : bx + bw;
    int y1 = ay + ah > by + bh ? ay + ah : by + bh;
    const int m = WM_SHADOW_SIZE + 12;
    wm_damage(x0 - m, y0 - m, (x1 - x0) + 2 * m, (y1 - y0) + 2 * m);
}

static int wm_anim_phase_q8(const Window* win, unsigned int now) {
    if (win->anim_kind == 0 || win->anim_duration_ms == 0) return 256;
    unsigned int elapsed = now - win->anim_start_ms;
    if (elapsed >= win->anim_duration_ms) return 256;
    return (int)((elapsed * 256u) / win->anim_duration_ms);
}

// Ease-out cubic: f(t) = 1 - (1-t)^3.  In Q8 this is a single multiply
// chain.  Snappier than smoothstep for window animations.
static int wm_ease_q8(int p_q8) {
    if (p_q8 <= 0)   return 0;
    if (p_q8 >= 256) return 256;
    int inv  = 256 - p_q8;
    int inv2 = (inv * inv) >> 8;
    int inv3 = (inv2 * inv) >> 8;
    int v = 256 - inv3;
    if (v < 0) v = 0; else if (v > 256) v = 256;
    return v;
}

// ── spring motion (compositor.motion = spring) ─────────────────────────────
// every animated window is drawn from a clean premultiplied snapshot at an
// eased visual rect, scale and opacity, while its real geometry has already
// changed, so hit-testing and input never wait for an animation. springs are
// solved in closed form against the microsecond clock: the same curve at 60,
// 144 or 240 hz. (satoru)
static bool wm_spring_on() { return wm_spring; }

static unsigned long long wm_now_us() { return FramePacer::NowNs() / 1000ull; }
static bool wm_anim_on() {
    return comp_animations_enabled && !comp_reduced_motion && comp_anim_duration_ms > 0;
}
// spring response scaled by compositor.animation_speed_ms (180 = as designed). (satoru)
static float wm_resp(float r) { return r * (float)wm_speed_q8() / 256.0f; }

static int wm_radius(const Window* w) {
    return w->state == WIN_FULLSCREEN ? 0 : WM_CORNER_RADIUS;
}

static void wm_fx_old_release(Window* w) {
    WMMotion::Snapshot s; s.px = w->fx_old_px; s.w = w->fx_old_w; s.h = w->fx_old_h;
    WMMotion::Release(s);
    w->fx_old_px = nullptr; w->fx_old_w = w->fx_old_h = 0;
}

static void wm_fx_init(Window* w) {
    WMMotion::SpringSet(w->fx_sc, 1.0f);
    WMMotion::SpringSet(w->fx_op, 1.0f);
    WMMotion::SpringSet(w->fx_gn, 0.0f);
    WMMotion::SpringSet(w->fx_el, w->focused ? 1.0f : 0.0f);
    for (int k = 0; k < 4; k++) WMMotion::SpringSet(w->fx_r[k], 0.0f);
    w->fx_us = 0;
    w->fx_mode = FX_NONE;
    w->fx_rect = false;
    w->fx_cap = false;
    w->fx_need_old = false;
    w->fx_caught = false;
    w->fx_o_tb = true;
    w->fx_ox = w->fx_oy = w->fx_ow = w->fx_oh = 0;
    w->fx_d0 = 1.0f;
    w->fx_bx0 = w->fx_by0 = w->fx_bx1 = w->fx_by1 = 0;
}

// the drawn rect of a window right now (before scale), in float. (satoru)
static void wm_fx_rect(const Window* w, float& x, float& y, float& ww, float& hh) {
    if (w->fx_rect) { x = w->fx_r[0].x; y = w->fx_r[1].x; ww = w->fx_r[2].x; hh = w->fx_r[3].x; }
    else { x = (float)w->x; y = (float)w->y; ww = (float)w->w; hh = (float)w->h; }
    if (ww < 1.0f) ww = 1.0f;
    if (hh < 1.0f) hh = 1.0f;
}

// start (or retarget) a geometry morph from the rect the window is drawn at
// now. the real geometry must already be the new one. (satoru)
static void wm_fx_morph(Window* w, int ox, int oy, int ow, int oh, bool old_tb) {
    if (!wm_spring || !wm_anim_on() || ow <= 0 || oh <= 0) return;
    if (w->fx_mode == FX_CLOSE || w->fx_mode == FX_MIN || w->fx_mode == FX_UNMIN) return;
    if (ox == w->x && oy == w->y && ow == w->w && oh == w->h) return;
    if (w->fx_mode == FX_MORPH && w->snap_px) {
        // interrupted mid-morph: what we were heading to becomes the old
        // picture, the springs keep their position and velocity. (satoru)
        wm_fx_old_release(w);
        w->fx_old_px = w->snap_px; w->fx_old_w = w->snap_w; w->fx_old_h = w->snap_h;
        w->snap_px = nullptr; w->snap_w = w->snap_h = 0;
        w->fx_need_old = false;
    } else {
        float fx, fy, fw, fh;
        fx = (float)ox; fy = (float)oy; fw = (float)ow; fh = (float)oh;
        WMMotion::SpringSet(w->fx_r[0], fx); WMMotion::SpringSet(w->fx_r[1], fy);
        WMMotion::SpringSet(w->fx_r[2], fw); WMMotion::SpringSet(w->fx_r[3], fh);
        wm_fx_old_release(w);
        w->fx_need_old = true;
        w->fx_ox = ox; w->fx_oy = oy; w->fx_ow = ow; w->fx_oh = oh; w->fx_o_tb = old_tb;
        if (w->fx_mode == FX_OPEN) { WMMotion::SpringSet(w->fx_sc, 1.0f); WMMotion::SpringSet(w->fx_op, 1.0f); }
    }
    w->fx_r[0].t = (float)w->x; w->fx_r[1].t = (float)w->y;
    w->fx_r[2].t = (float)w->w; w->fx_r[3].t = (float)w->h;
    float d = 0.0f;
    for (int k = 0; k < 4; k++) { float e = w->fx_r[k].x - w->fx_r[k].t; d += e < 0 ? -e : e; }
    w->fx_d0 = d > 1.0f ? d : 1.0f;
    w->fx_rect = true;
    w->fx_cap = true;
    w->fx_mode = FX_MORPH;
    w->anim_kind = 0;
    if (!w->fx_us) w->fx_us = wm_now_us();
    Graphics::MarkUIDirty();
}

// pick the window that should take focus when `leaving` goes away. (satoru)
static int wm_next_focus(const Window* wins, int leaving) {
    int best = -1, bz = -1;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        const Window& o = wins[i];
        if (o.state == WIN_CLOSED || o.state == WIN_MINIMIZED || !o.visible || o.id == leaving) continue;
        if (o.fx_mode == FX_CLOSE || o.fx_mode == FX_MIN) continue;
        if (o.z_order > bz) { bz = o.z_order; best = o.id; }
    }
    return best;
}

// frame-cost probe for spring motion: wm render + overlay time of every frame
// with a motion in flight, logged once per motion as
// "[wmfx] kinds=... frames=N avg_us=A max_us=M res=WxH". (satoru)
static unsigned long long wm_fx_c0 = 0, wm_fx_sum = 0, wm_fx_max = 0;
static unsigned int wm_fx_n = 0, wm_fx_kinds = 0;
static unsigned long long wm_fx_cap_c = 0, wm_fx_sh_c = 0, wm_fx_bl_c = 0, wm_fx_gn_c = 0;   // per-part cycles (satoru)
static bool wm_fx_frame = false;
static inline unsigned long long wm_rdtsc() { unsigned int lo, hi; __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi)); return ((unsigned long long)hi << 32) | lo; }
static void wm_fx_probe_end(unsigned long long add) {
    if (!wm_fx_frame) return;
    unsigned long long c = (wm_rdtsc() - wm_fx_c0) + add;
    wm_fx_sh_c += WMMotion::ProfTake(0);
    wm_fx_bl_c += WMMotion::ProfTake(1);
    wm_fx_gn_c += WMMotion::ProfTake(2);
    wm_fx_sum += c; if (c > wm_fx_max) wm_fx_max = c; wm_fx_n++;
}
static void wm_fx_probe_flush(int sw, int sh) {
    if (!wm_fx_n) return;
    unsigned long long f = CPUDetect::GetInfo().frequency.tsc_frequency / 1000000ull;
    if (!f) f = 2000;
    SerialLogger::Log("[wmfx] kinds=");
    SerialLogger::LogHex(wm_fx_kinds);
    SerialLogger::Log(" frames="); SerialLogger::LogDec((int)wm_fx_n);
    SerialLogger::Log(" avg_us="); SerialLogger::LogDec((int)(wm_fx_sum / wm_fx_n / f));
    SerialLogger::Log(" max_us="); SerialLogger::LogDec((int)(wm_fx_max / f));
    SerialLogger::Log(" res="); SerialLogger::LogDec(sw); SerialLogger::Log("x"); SerialLogger::LogDec(sh);
    // per frame averages of the parts: capture, shadows, scaled blits, genie (satoru)
    SerialLogger::Log(" cap="); SerialLogger::LogDec((int)(wm_fx_cap_c / wm_fx_n / f));
    SerialLogger::Log(" shadow="); SerialLogger::LogDec((int)(wm_fx_sh_c / wm_fx_n / f));
    SerialLogger::Log(" blit="); SerialLogger::LogDec((int)(wm_fx_bl_c / wm_fx_n / f));
    SerialLogger::Log(" genie="); SerialLogger::LogDec((int)(wm_fx_gn_c / wm_fx_n / f));
    SerialLogger::Log("\r\n");
    wm_fx_cap_c = wm_fx_sh_c = wm_fx_bl_c = wm_fx_gn_c = 0;
    wm_fx_n = 0; wm_fx_sum = wm_fx_max = 0; wm_fx_kinds = 0;
}
static unsigned long long wm_fx_render_c = 0;   // this frame's Render() share (satoru)

void WindowManager::Render() {
    // First retire any animations that have completed since the last
    // tick so windows in their final CLOSE/MINIMIZE phase are dropped
    // before we build the visibility list.
    TickAnimations();
    wm_fx_frame = false;
    if (wm_spring) {
        for (int i = 0; i < WM_MAX_WINDOWS; i++)
            if (windows[i].state != WIN_CLOSED && windows[i].fx_mode) {
                wm_fx_frame = true;
                wm_fx_kinds |= 1u << windows[i].fx_mode;
            }
        if (!wm_fx_frame) wm_fx_probe_flush(screen_width, screen_height);
        if (!wm_fx_frame) { WMMotion::ProfTake(0); WMMotion::ProfTake(1); WMMotion::ProfTake(2); }
    }
    wm_fx_c0 = wm_rdtsc();

    // render windows in z-order (lowest first)
    // build sorted index
    int indices[WM_MAX_WINDOWS];
    int count = 0;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        const Window& w = windows[i];
        if (w.state != WIN_CLOSED && w.visible && w.state != WIN_MINIMIZED)
            indices[count++] = i;
    }

    // sort by z-order - insertion sort (count small, near-sorted)
    for (int i = 1; i < count; i++) {
        int key = indices[i];
        int key_z = windows[key].z_order;
        int j = i - 1;
        while (j >= 0 && windows[indices[j]].z_order > key_z) {
            indices[j+1] = indices[j];
            j--;
        }
        indices[j+1] = key;
    }

    // occlusion cull: drop any window fully covered by an opaque,
    // non-animated window above it.  Anim alpha < 255 would let the
    // covered window show through, so animated covers don't cull.
    bool draw[WM_MAX_WINDOWS];
    for (int i = 0; i < count; i++) draw[i] = true;
    for (int i = 0; i < count - 1; i++) {
        const Window* w = &windows[indices[i]];
        for (int j = i + 1; j < count; j++) {
            const Window* o = &windows[indices[j]];
            if (o->anim_kind != 0 || o->alpha < 255 || o->state == WIN_FULLSCREEN) continue;
            if (o->fx_mode != 0) continue;   // drawn eased, may not cover its real rect (satoru)
            // when capturing the drag backdrop the dragged window is NOT drawn,
            // so it must not occlude windows behind it - otherwise they'd be
            // missing from the backdrop and vanish once the window slides off
            // them. (satoru)
            if (WM_UNLIKELY(wm_backdrop_capturing && o->id == wm_backdrop_win_id)) continue;
            // o covers w iff o's rect contains w's rect.
            if (o->x <= w->x && o->y <= w->y &&
                o->x + o->w >= w->x + w->w &&
                o->y + o->h >= w->y + w->h) {
                draw[i] = false;
                break;
            }
        }
    }

    // the window drawn last (on top) this frame, for move by copy (satoru)
    wm_last_top_id = count > 0 ? windows[indices[count - 1]].id : -1;

    // render each window
    bool dragging = IsDragging();
    unsigned int now = Timer::GetRealMs();
    for (int i = 0; i < count; i++) {
        Window* win = &windows[indices[i]];

        // Persist render-time geometry so future moves/resizes can damage
        // the prior footprint even if Update() didn't update last_*.
        win->last_x = win->x;
        win->last_y = win->y;
        win->last_w = win->w;
        win->last_h = win->h;
        win->had_last = true;

        if (!draw[i]) {
            win->dirty = false;
            continue;
        }

        // while snapshotting the drag backdrop, omit the dragged window so
        // the captured frame contains only what sits BEHIND it. last_* was
        // still refreshed above so move/close damage stays correct. (satoru)
        if (WM_UNLIKELY(wm_backdrop_capturing && win->id == wm_backdrop_win_id)) {
            win->dirty = false;
            continue;
        }

        // spring motion: genie windows draw in the overlay pass (after the
        // dock); open/close/morph draw from their snapshots here. (satoru)
        if (wm_spring && win->fx_mode != FX_NONE) {
            if (win->fx_mode == FX_MIN || win->fx_mode == FX_UNMIN) {
                if (win->fx_cap) {
                    FxCapture(win, win->x, win->y, win->w, win->h, win->has_titlebar,
                              &win->snap_px, &win->snap_w, &win->snap_h);
                    win->fx_cap = false;
                }
            } else {
                RenderFxWindow(win);
            }
            win->dirty = false;
            continue;
        }

        // ── animation phase (Q8 fixed-point, eased) ──────────────────
        int p_raw = wm_anim_phase_q8(win, now);
        int p     = wm_ease_q8(p_raw);
        // p_vis maps to "how visible should we be" depending on kind:
        //   open    : 0 -> 256
        //   close   : 256 -> 0
        //   minimize: 256 -> 0   (window also shrinks toward anchor)
        //   restore : 0 -> 256
        unsigned char kind = win->anim_kind;
        int p_vis = (kind == 2 || kind == 3) ? (256 - p) : p;
        if (kind == 0) p_vis = 256;

        // window motion. minimize (3) and unminimize (5) pour into and out of
        // the taskbar tile, so RenderMotionOverlay() draws them after the
        // taskbar; here minimize only captures its snapshot on the first
        // frame (that frame still shows the whole window). (satoru)
        if (WM_UNLIKELY((kind == 3 || kind == 5) && p_raw < 256)) {
            if (kind == 3 && !win->snap_px) {
                RenderWindowBody(win, (!dragging || comp_shadow_during_drag));
                wm_tab_thumb_grab(win);
                wm_snap_capture(win);
            }
            wm_damage(win->x - WM_SHADOW_SIZE, win->y - WM_SHADOW_SIZE,
                      win->w + 2*WM_SHADOW_SIZE, win->h + 2*WM_SHADOW_SIZE);
            win->dirty = false;
            continue;
        }
        if (WM_UNLIKELY(kind == 1 && p_raw < 256)) {
            RenderOpenMotion(win, wm_motion_t(win, now));
            win->dirty = false;
            continue;
        }
        if (WM_UNLIKELY(kind == 2 && p_raw < 256)) {
            if (!win->snap_px) {
                // first close frame: draw the window as it stands and keep it.
                RenderWindowBody(win, (!dragging || comp_shadow_during_drag));
                wm_snap_capture(win);
                wm_damage(win->x - WM_SHADOW_SIZE, win->y - WM_SHADOW_SIZE,
                          win->w + 2*WM_SHADOW_SIZE, win->h + 2*WM_SHADOW_SIZE);
            } else {
                RenderCloseMotion(win, wm_motion_t(win, now));
            }
            win->dirty = false;
            continue;
        }
        if (WM_UNLIKELY(kind == 4 && p_raw < 256)) {
            int q = p;                             // 0..256
            int ax = win->x + win->w / 2, ay = screen_height - 22;  // live anchor, not stale create-time (satoru)
            int rx = ax + ((win->x - ax) * q) / 256;
            int ry = ay + ((win->y - ay) * q) / 256;
            int rw = (win->w * q) / 256;
            int rh = (win->h * q) / 256;
            if (rw < 4)  rw = 4;
            if (rh < 4)  rh = 4;
            unsigned int outline = 0xFF6C8CFFu;
            Graphics::DrawRect(rx, ry, rw, rh, outline);
            wm_damage(rx, ry, rw, rh);
            win->dirty = false;
            continue;
        }

        // kurono.gfx.scissor=1: a settled window wholly outside this pass's
        // region draws nothing. its content callback still runs, under an
        // empty clip, so app-side state (timers, frame requests, self-damage)
        // advances exactly as before. the margin covers the shadow. (satoru)
        {
            int sx, sy, sw, sh;
            const int m = 64;
            if (Graphics::GetScissor(sx, sy, sw, sh) &&
                (win->x - m >= sx + sw || win->y - m >= sy + sh ||
                 win->x + win->w + m <= sx || win->y + win->h + m <= sy)) {
                // the focus border ease damages its edges while it draws them;
                // keep that, so the ease is never left half done. (satoru)
                uint32_t bt = win->focused ? wm_get_accent() : COL_BORDER;
                uint32_t bc = KSS::Anim::Color(KSS::Motion::Id((uint32_t)win->id, 0x30u),
                                               bt, KSS::Motion::Window, KSS::Motion::Std);
                if (bc != bt && !wm_in_fast_render) {
                    wm_damage(win->x, win->y, win->w, 1);
                    wm_damage(win->x, win->y + win->h - 1, win->w, 1);
                    wm_damage(win->x, win->y, 1, win->h);
                    wm_damage(win->x + win->w - 1, win->y, 1, win->h);
                    wm_damage_corners(win);
                }
                if (win->render && win->content_w > 0 && win->content_h > 0) {
                    Graphics::PushClipRect(win->content_x, win->content_y, 0, 0);
                    { TaskMem::WindowScope _tms(win->id); win->render(win, win->content_x, win->content_y, win->content_w, win->content_h); }
                    Graphics::PopClipRect();
                }
                win->dirty = false;
                continue;
            }
        }

        // shadow + body + titlebar + border + content. shadow is skipped
        // during drag/resize unless the user opted in. shared helper keeps the
        // dragged-only fast path pixel-identical to this loop. (satoru)
        RenderWindowBody(win, (!dragging || comp_shadow_during_drag));
        wm_tab_thumb_grab(win);

        // ── post-pass: per-window opacity overlay (animation done) ──
        int combined = (int)win->alpha * p_vis / 256;
        if (combined < 255) {
            unsigned int dim = (unsigned int)(255 - combined);
            Graphics::FillRectAlpha(win->x, win->y, win->w, win->h, (uint8_t)dim, 0xFF000000u);
        }

        // Animated windows must keep redrawing until phase reaches 1.
        if (kind != 0 && p_raw < 256) {
            wm_damage(win->x - WM_SHADOW_SIZE, win->y - WM_SHADOW_SIZE,
                      win->w + 2*WM_SHADOW_SIZE, win->h + 2*WM_SHADOW_SIZE);
        }

        win->dirty = false;
    }

    // window context menu draws on top of everything (satoru)
    RenderContextMenu();
    wm_fx_render_c = wm_fx_frame ? wm_rdtsc() - wm_fx_c0 : 0;
}

// draw one settled window: shadow (optional) + rounded body + titlebar +
// border + clipped content. factored out of Render()'s z-order loop so the
// dragged-only fast path produces identical pixels. assumes the window is not
// mid-animation (anim_kind==0), which holds for a window being dragged. (satoru)
void WindowManager::RenderWindowBody(Window* win, bool with_shadow) {
    if (!win) return;
    if (with_shadow) RenderShadow(win);

    // focus<->unfocus border: ease between the accent (focused) and the plain
    // border so gaining/losing focus reads as a subtle elevation change rather
    // than a hard snap. keyed per window so each keeps its own state. (satoru)
    uint32_t border_target = win->focused ? wm_get_accent() : COL_BORDER;
    uint32_t border = KSS::Anim::Color(KSS::Motion::Id((uint32_t)win->id, 0x30u),
                                       border_target, KSS::Motion::Window, KSS::Motion::Std);
    // while the border colour is still easing toward its target, damage just the
    // four edge strips so the present stays partial instead of re-copying the
    // whole frame for a 220ms colour fade. Focus() already raised the initial
    // dirty signal + edge damage; this scopes the trailing ease frames. (satoru)
    if (border != border_target && !wm_in_fast_render) {
        wm_damage(win->x, win->y, win->w, 1);
        wm_damage(win->x, win->y + win->h - 1, win->w, 1);
        wm_damage(win->x, win->y, 1, win->h);
        wm_damage(win->x + win->w - 1, win->y, 1, win->h);
        wm_damage_corners(win);
    }

    // full body background with rounded corners (anti-aliased in modern)
    const int wrad = wm_radius(win);   // full screen windows are square (satoru)
    if (UIStyle::Modern())
        MDraw::RoundRect(win->x, win->y, win->w, win->h, wrad, win->bg_color, 255);
    else
        Graphics::FillRoundedRect(win->x, win->y, win->w, win->h, wrad, win->bg_color);

    // titlebar (draws over the top portion)
    if (win->has_titlebar) RenderTitlebar(win);

    // border: a faint hairline in modern (focus reads from the traffic lights
    // and shadow), the accent outline in classic.
    if (UIStyle::Modern())
        { if (wrad > 0) MDraw::RoundOutline(win->x, win->y, win->w, win->h, wrad,
                            wm_is_light(win->bg_color) ? 0xFF000000u : 0xFFFFFFFFu, win->focused ? 46 : 26); }
    else
        Graphics::DrawRect(win->x, win->y, win->w, win->h, border);

    // content (clipped to content area so text doesn't bleed outside when the
    // user shrinks the window below the content's natural size)
    if (win->render && win->content_w > 0 && win->content_h > 0) {
        Graphics::PushClipRect(win->content_x, win->content_y,
                              win->content_w, win->content_h);
        { TaskMem::WindowScope _tms(win->id); win->render(win, win->content_x, win->content_y, win->content_w, win->content_h); }
        Graphics::PopClipRect();
    }
}

// ─────────────────────────────────────────────────────────────────────
//  Cached-desktop snapshot during a window drag. (satoru)
// ─────────────────────────────────────────────────────────────────────

// true only for an active left-button TITLEBAR drag (not a resize). resize is
// deliberately excluded - its content relayout wants the full render. (satoru)
bool WindowManager::IsWindowDragActive() {
    return mouse_is_down && current_action == WM_DRAG && action_window_id > 0;
}

bool WindowManager::DragBackdropReady() {
    if (!wm_backdrop_ready || !wm_backdrop) return false;
    // a stale backdrop (resolution change or too old) must be rebuilt so the
    // wallpaper/taskbar behind the window can't freeze on screen. compare to
    // the live framebuffer geometry the snapshot was sized from. (satoru)
    if (wm_backdrop_w != Graphics::GetWidth() ||
        wm_backdrop_h != Graphics::GetHeight()) return false;
    if (wm_backdrop_pitch != Graphics::GetPitch()) return false;
    if ((Timer::GetRealMs() - wm_backdrop_built_ms) > WM_BACKDROP_MAX_AGE_MS) return false;
    return true;
}

void WindowManager::InvalidateDragBackdrop() {
    wm_backdrop_ready     = false;
    wm_backdrop_capturing = false;
    wm_backdrop_win_id    = -1;
    wm_drag_prev_valid    = false;
    wm_drag_cur_valid     = false;
}

// arm the next full render to leave the dragged window out so we can snapshot
// what is behind it. no-op if there is no active titlebar drag. (satoru)
void WindowManager::BeginDragCapture() {
    if (!IsWindowDragActive()) { wm_backdrop_capturing = false; return; }
    wm_backdrop_capturing = true;
    wm_backdrop_win_id    = action_window_id;
}

// (re)allocate the backdrop buffer to the current screen geometry. sized from
// the ACTUAL framebuffer (graphics) so the snapshot copy can never over/under-
// run the back buffer if wm's screen_* ever drift from the fb. returns false
// if allocation failed or geometry is unusable. (satoru)
static bool wm_backdrop_alloc() {
    int w = Graphics::GetWidth();
    int h = Graphics::GetHeight();
    uint32_t pitch = Graphics::GetPitch();
    if (w <= 0 || h <= 0 || pitch == 0) return false;
    size_t need = (size_t)pitch * (size_t)h;
    if (wm_backdrop && (wm_backdrop_bytes != need)) {
        TaskMem::UnchargeFrames(wm_backdrop); PMM::FreeBytes(wm_backdrop, wm_backdrop_bytes);
        wm_backdrop = nullptr;
        wm_backdrop_bytes = 0;
    }
    if (!wm_backdrop) {
        wm_backdrop = (uint8_t*)PMM::AllocBytes(need); TaskMem::ChargeFrames(wm_backdrop, need);
        if (!wm_backdrop) { wm_backdrop_bytes = 0; return false; }
        wm_backdrop_bytes = need;
    }
    wm_backdrop_w     = w;
    wm_backdrop_h     = h;
    wm_backdrop_pitch = pitch;
    return true;
}

// snapshot the just-composed (dragged-window-excluded) back buffer into the
// backdrop. one straight copy of cached RAM. clears the capture skip. (satoru)
void WindowManager::CaptureDragBackdrop() {
    wm_backdrop_capturing = false;          // capture frame is over either way
    if (!IsWindowDragActive()) { InvalidateDragBackdrop(); return; }
    if (!wm_backdrop_alloc()) { wm_backdrop_ready = false; return; }

    uint8_t* back = Graphics::GetBackBuffer();
    if (!back) { wm_backdrop_ready = false; return; }
    // copy the rows actually backed by the framebuffer pitch. (satoru)
    memcpy(wm_backdrop, back, (size_t)wm_backdrop_pitch * (size_t)wm_backdrop_h);

    wm_backdrop_win_id = action_window_id;
    wm_backdrop_ready  = true;
    wm_backdrop_built_ms = Timer::GetRealMs();
    // seed the previous footprint with the window as it stood in the snapshot
    // frame so the first fast frame damages a correct prev rect. (satoru)
    Window* w = GetWindow(action_window_id);
    if (w) {
        wm_drag_prev_x = w->x; wm_drag_prev_y = w->y;
        wm_drag_prev_w = w->w; wm_drag_prev_h = w->h;
        wm_drag_prev_valid = true;
    } else {
        wm_drag_prev_valid = false;
    }
}

// blit a rectangular block from the backdrop back into the live back buffer.
// rect is clipped to the screen; copies row-by-row at the captured pitch.
// (satoru)
static void wm_backdrop_blit(int x, int y, int w, int h) {
    if (!wm_backdrop) return;
    int sw = wm_backdrop_w, sh = wm_backdrop_h;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= sw || y >= sh) return;
    if (x + w > sw) w = sw - x;
    if (y + h > sh) h = sh - y;
    if (w <= 0 || h <= 0) return;
    uint8_t* back = Graphics::GetBackBuffer();
    if (!back) return;
    uint32_t pitch = wm_backdrop_pitch;
    size_t   row_bytes = (size_t)w * 4u;       // 32bpp rgba
    size_t   col_off   = (size_t)x * 4u;
    for (int row = y; row < y + h; row++) {
        size_t off = (size_t)row * pitch + col_off;
        memcpy(back + off, wm_backdrop + off, row_bytes);
    }
}

// footprint margin around the dragged window for backdrop-restore + damage.
// must cover (a) the mouse cursor, which is drawn after Render and can reach
// ~16px past the grab point when the titlebar is grabbed near an edge, and
// (b) the soft shadow when shadows are kept during drag (reaches
// comp_shadow_radius+3 beyond the body). a stale margin would leave a cursor
// or shadow trail since only this region is swapped. (satoru)
#define WM_DRAG_CURSOR_MARGIN 18
static inline int wm_drag_margin() {
    int m = WM_DRAG_CURSOR_MARGIN;
    if (comp_shadow_during_drag) {
        int s = comp_shadow_radius + 4;        // +3 focus bump, +1 slop (satoru)
        if (s > m) m = s;
    }
    return m;
}

// fast drag frame: restore the backdrop only where the window was and is, then
// redraw the moving window on top, damaging just that union so SwapBuffers
// copies a small region. avoids the full wallpaper + every-window re-composite.
// (satoru)
void WindowManager::RenderDragFast() {
    Window* win = GetWindow(action_window_id);
    if (!win || !DragBackdropReady()) return;

    const int m = wm_drag_margin();            // cover the soft shadow too

    // the damage we raise below is the moving window's own footprint, NOT a
    // structural change - keep wm_damage from dropping the very backdrop we
    // are using. (satoru)
    wm_in_fast_render = true;

    // 1) repaint the previous footprint from the clean backdrop so the window
    //    leaves no trail where it used to be. (satoru)
    if (wm_drag_prev_valid) {
        wm_backdrop_blit(wm_drag_prev_x - m, wm_drag_prev_y - m,
                         wm_drag_prev_w + 2*m, wm_drag_prev_h + 2*m);
        wm_damage(wm_drag_prev_x - m, wm_drag_prev_y - m,
                  wm_drag_prev_w + 2*m, wm_drag_prev_h + 2*m);
    }

    // 2) lay the backdrop under the window's CURRENT spot (so the shadow,
    //    rounded corners and any translucency composite over real pixels, not
    //    over the previous frame's window). (satoru)
    wm_backdrop_blit(win->x - m, win->y - m, win->w + 2*m, win->h + 2*m);

    // 3) draw the moving window itself, shadow honouring the drag setting. (satoru)
    RenderWindowBody(win, comp_shadow_during_drag);

    // 4) damage the window's current footprint (+shadow) for the partial swap. (satoru)
    wm_damage(win->x - m, win->y - m, win->w + 2*m, win->h + 2*m);

    // 5) cursor trail guard: the OS redraws the cursor (12x16, hotspot at the
    //    top-left) after this returns. when the pointer detaches from the
    //    window at a clamp edge it can sit outside the window rect, so restore
    //    the backdrop under the previous AND current cursor box and damage both
    //    - otherwise only the window region swaps and the cursor smears. boxes
    //    that fall inside the window rect just re-copy harmlessly. (satoru)
    {
        const int cw = 12, ch = 16, cs = 2;     // cursor size + slop (satoru)
        int cx = Mouse::mx, cy = Mouse::my;
        if (wm_drag_cur_valid) {
            wm_backdrop_blit(wm_drag_cur_px - cs, wm_drag_cur_py - cs,
                             cw + 2*cs, ch + 2*cs);
            wm_damage(wm_drag_cur_px - cs, wm_drag_cur_py - cs,
                      cw + 2*cs, ch + 2*cs);
        }
        wm_backdrop_blit(cx - cs, cy - cs, cw + 2*cs, ch + 2*cs);
        wm_damage(cx - cs, cy - cs, cw + 2*cs, ch + 2*cs);
        wm_drag_cur_px = cx; wm_drag_cur_py = cy; wm_drag_cur_valid = true;
    }

    // remember where it is for next frame's prev-rect repaint. (satoru)
    win->last_x = win->x; win->last_y = win->y;
    win->last_w = win->w; win->last_h = win->h;
    win->had_last = true;
    wm_drag_prev_x = win->x; wm_drag_prev_y = win->y;
    wm_drag_prev_w = win->w; wm_drag_prev_h = win->h;
    wm_drag_prev_valid = true;

    // the wm context menu can't be open during a drag, but keep parity with
    // Render() in case that ever changes. (satoru)
    RenderContextMenu();

    wm_in_fast_render = false;
}

// draw only the dragged window over the current back buffer - used right after
// the backdrop snapshot so the capture frame is itself complete. (satoru)
void WindowManager::RenderDraggedWindowOnly() {
    Window* win = GetWindow(action_window_id);
    if (!win) return;
    // guard so this window's own damage doesn't drop the backdrop that
    // CaptureDragBackdrop just marked ready. (satoru)
    wm_in_fast_render = true;
    RenderWindowBody(win, comp_shadow_during_drag);
    const int m = wm_drag_margin();
    wm_damage(win->x - m, win->y - m, win->w + 2*m, win->h + 2*m);
    win->last_x = win->x; win->last_y = win->y;
    win->last_w = win->w; win->last_h = win->h;
    win->had_last = true;
    wm_in_fast_render = false;
}

// the rounded outline's arcs sit inside the corner squares, off the 1 px edge
// strips the focus ease damages, so with damage-only rendering a focus change
// left the old arcs. damage the four corner squares too. (satoru)
static void wm_damage_corners(const Window* win) {
    if (!Graphics::ScissorEnabled() && !Graphics::WallpaperDamageOnly()) return;
    int r = wm_radius(win) + 2;
    wm_damage(win->x, win->y, r, r);
    wm_damage(win->x + win->w - r, win->y, r, r);
    wm_damage(win->x, win->y + win->h - r, r, r);
    wm_damage(win->x + win->w - r, win->y + win->h - r, r, r);
}

// window shadows are not drawn while a drag or resize runs, for every window.
// with damage-only rendering (kurono.gfx.scissor=1, kurono.wpdamage=1) that
// switch only reached the screen where some other damage happened to fall,
// so shadows were cut into bands at the start and end of a drag. damage every
// shadowed window when it flips. (satoru)
static void wm_damage_all_shadows() {
    if (comp_shadow_during_drag || !comp_shadow_enabled) return;
    if (!Graphics::ScissorEnabled() && !Graphics::WallpaperDamageOnly()) return;
    const int m = WM_SHADOW_SIZE + 2;
    Window* ws = WindowManager::GetWindows();
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        const Window* w = &ws[i];
        if (w->state == WIN_CLOSED || !w->visible || w->state == WIN_MINIMIZED || w->state == WIN_FULLSCREEN) continue;
        wm_damage(w->x - m, w->y - m, w->w + 2 * m, w->h + 2 * m);
    }
}

bool WindowManager::HandleMouseDown(int mx, int my) {
    mouse_is_down = true;
    int top_id = TopWindowAt(mx, my);
    if (top_id > 0) {
        Focus(top_id);
        Window* win = GetWindow(top_id);
        if (!win) return true;

        // macos traffic light buttons on left side
        int btn_cy = win->y + WM_TITLEBAR_H / 2;
        int btn_start_x = win->x + 14;
        const int btn_r2 = 8 * 8;

        if (win->closable) {
            int dx = mx - btn_start_x;
            int dy = my - btn_cy;
            if (dx*dx + dy*dy <= btn_r2) { UserCloseWindow(top_id); return true; }
        }
        {
            int dx = mx - (btn_start_x + 22);
            int dy = my - btn_cy;
            if (dx*dx + dy*dy <= btn_r2) { Minimize(top_id); return true; }
        }
        {
            int dx = mx - (btn_start_x + 44);
            int dy = my - btn_cy;
            if (dx*dx + dy*dy <= btn_r2) { ToggleMaximize(top_id); return true; }
        }
        WMAction action = HitTest(win, mx, my);
        if (action != WM_NONE) {
            if (current_action == WM_NONE) wm_damage_all_shadows();   // shadows go off (satoru)
            current_action = action;
            action_window_id = top_id;
            drag_offset_x = mx - win->x;
            drag_offset_y = my - win->y;
            wm_last_drag_ms = Timer::GetRealMs();
            wm_input_capture_id = -1;
            return true;
        }
        if (win->input &&
            mx >= win->content_x && mx <  win->content_x + win->content_w &&
            my >= win->content_y && my <  win->content_y + win->content_h) {
            wm_input_capture_id = top_id;
            { TaskMem::WindowScope _tms(win->id); win->input(win, 1, mx - win->content_x, my - win->content_y); }
        }
        return true;
    }
    wm_input_capture_id = -1;
    return false;
}

void WindowManager::HandleMouseMove(int mx, int my) {
    if (!mouse_is_down || current_action == WM_NONE) return;
    Window* win = GetWindow(action_window_id);
    if (!win) { current_action = WM_NONE; action_window_id = -1; wm_snap_preview_clear(); return; }

    // the snap target follows the pointer even when the window move itself is
    // throttled to the refresh interval. (satoru)
    if (current_action == WM_DRAG) wm_snap_preview_update(win, current_action, mx, my);

    unsigned int now = Timer::GetRealMs();
    if (current_action == WM_DRAG && wm_drag_min_gap_ms > 0
        && (now - wm_last_drag_ms) < wm_drag_min_gap_ms) {
        return;
    }
    wm_last_drag_ms = now;

    int ox = win->x, oy = win->y, ow = win->w, oh = win->h;
    if (wm_apply_action(win, current_action, mx, my,
                        drag_offset_x, drag_offset_y)) {
        wm_fp_damage(ox - WM_SHADOW_SIZE, oy - WM_SHADOW_SIZE,
                     ow + 2*WM_SHADOW_SIZE, oh + 2*WM_SHADOW_SIZE);
        wm_fp_damage(win->x - WM_SHADOW_SIZE, win->y - WM_SHADOW_SIZE,
                     win->w + 2*WM_SHADOW_SIZE, win->h + 2*WM_SHADOW_SIZE);
        UpdateContentArea(win);
        win->dirty = true;
    }
}

void WindowManager::HandleMouseUp(int mx, int my) {
    // edge-snap: if the user released a drag near a screen edge, snap.
    Window* acted = (current_action != WM_NONE) ? GetWindow(action_window_id) : nullptr;

    if (current_action == WM_DRAG && acted) {
        // refresh the target for the release point, then snap into that
        // output's work area. no edge under the pointer means a plain move.
        // (satoru)
        wm_snap_preview_update(acted, current_action, mx, my);
        if (acted->resizable && wm_sp_edge >= 0 && wm_sp_win == acted->id) {
            ApplySnap(acted, wm_sp_edge, wm_sp_ax, wm_sp_ay, wm_sp_aw, wm_sp_ah);
        } else {
            acted->dirty = true;
            wm_geom_remember(acted, true);
        }
    } else if (acted) {
        acted->dirty = true;
        wm_geom_remember(acted, true);
    }
    wm_snap_preview_clear();

    // a finished drag/resize may have moved the window onto another output (satoru)
    if (acted) UpdateWindowMonitor(acted);

    if (current_action != WM_NONE) wm_damage_all_shadows();   // shadows come back (satoru)
    mouse_is_down = false;
    current_action = WM_NONE;
    action_window_id = -1;
    // drag is over - drop the snapshot so the next drag starts clean and the
    // released window gets a normal full render this frame. (satoru)
    InvalidateDragBackdrop();
    (void)mx; (void)my;
}

// which traffic-light button (0=close,1=min,2=max) is (mx,my) over for `win`,
// or -1 if none. mirrors the geometry in RenderTitlebar/the click handler:
// centers at btn_start_x + {0,22,44}, click radius 8. (satoru)
static int wm_titlebar_button_at(const Window* win, int mx, int my) {
    if (!win || !win->has_titlebar) return -1;
    int btn_cy = win->y + WM_TITLEBAR_H / 2;
    int btn_start_x = win->x + 14;
    const int r2 = 8 * 8;
    const int cx[3] = { btn_start_x, btn_start_x + 22, btn_start_x + 44 };
    for (int i = 0; i < 3; i++) {
        if (i == 0 && !win->closable) continue;
        int dx = mx - cx[i], dy = my - btn_cy;
        if (dx * dx + dy * dy <= r2) return i;
    }
    return -1;
}

// update which titlebar button the top window's pointer is over; mark dirty on a
// change so the hover ease repaints. called from the wm update path with the
// live pointer even when no app owns the cursor. (satoru)
void WindowManager::UpdateTitlebarHover(int mx, int my) {
    int top_id = (current_action == WM_NONE) ? TopWindowAt(mx, my) : -1;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        Window* w = &windows[i];
        if (w->state == WIN_CLOSED) continue;
        int hb = (w->id == top_id) ? wm_titlebar_button_at(w, mx, my) : -1;
        if (hb != w->tb_hover_btn) {
            w->tb_hover_btn = (signed char)hb;
            Graphics::MarkUIDirty();   // hover ease needs to render (satoru)
        }
    }
}

void WindowManager::HandlePointerMove(int mx, int my) {
    // keep the titlebar traffic-light hover state fresh even while the pointer
    // isn't over an app's content area. (satoru)
    UpdateTitlebarHover(mx, my);
    // While dragging/resizing a window, suppress hover input to apps so
    // they don't react to the pointer slipping over them.
    if (current_action != WM_NONE) return;

    Window* win = nullptr;
    if (wm_input_capture_id > 0) {
        win = GetWindow(wm_input_capture_id);
        if (!win || !win->visible || win->state == WIN_MINIMIZED) {
            wm_input_capture_id = -1;
            win = nullptr;
        }
    }

    if (!win) {
        int top_id = TopWindowAt(mx, my);
        if (top_id > 0) win = GetWindow(top_id);
    }

    if (!win || !win->input) return;
    if (wm_input_capture_id < 0 &&
        (mx < win->content_x || mx >= win->content_x + win->content_w ||
         my < win->content_y || my >= win->content_y + win->content_h)) {
        return;
    }

    { TaskMem::WindowScope _tms(win->id); win->input(win, 5, mx - win->content_x, my - win->content_y); }
}

void WindowManager::HandlePointerButton(int mx, int my, int button, bool pressed) {
    if (button < 0 || button > 4) return;

    Window* win = nullptr;
    if (!pressed && wm_input_capture_id > 0) {
        win = GetWindow(wm_input_capture_id);
    }

    if (!win) {
        int top_id = TopWindowAt(mx, my);
        if (top_id > 0) {
            win = GetWindow(top_id);
            if (pressed) Focus(top_id);
        }
    }

    if (!win || !win->input) {
        if (!pressed) wm_input_capture_id = -1;
        return;
    }

    if (pressed) {
        if (mx < win->content_x || mx >= win->content_x + win->content_w ||
            my < win->content_y || my >= win->content_y + win->content_h) {
            return;
        }
        wm_input_capture_id = win->id;
    }

    { TaskMem::WindowScope _tms(win->id); win->input(win, 6, button, pressed ? 1 : 0); }

    if (!pressed && wm_input_capture_id == win->id) {
        wm_input_capture_id = -1;
    }
}

void WindowManager::RenderAll() {
    Render();
}

bool WindowManager::IsDragging() {
    return mouse_is_down && current_action != WM_NONE;
}

bool WindowManager::HasActiveAnimations() {
    if (wm_tab_open || wm_sp_edge >= 0) return true;
    unsigned int now = Timer::GetRealMs();
    if (wm_spring) {
        for (int i = 0; i < WM_MAX_WINDOWS; i++) {
            const Window* w = &windows[i];
            if (w->state == WIN_CLOSED) continue;
            if (w->catch_kind && (int)(now - w->catch_ms) < (int)wm_motion_dur(520)) return true;
            if (w->fx_mode != FX_NONE) return true;
            if (w->state != WIN_MINIMIZED && !WMMotion::SpringRest(w->fx_el, 0.002f)) return true;
        }
        return false;
    }
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        const Window* w = &windows[i];
        if (w->state == WIN_CLOSED) continue;
        // a taskbar tile catch outlives the minimize that triggered it.
        if (w->catch_kind && (int)(now - w->catch_ms) < (int)wm_motion_dur(520)) return true;
        if (w->anim_kind == 0) continue;
        if (w->anim_duration_ms == 0) continue;
        if ((now - w->anim_start_ms) < w->anim_duration_ms) return true;
    }
    return false;
}

// ── window motion renderers ─────────────────────────────────────────
using WMMotion::Lerp;
using WMMotion::Bezier;

// open: the taskbar tile becomes the window. four independent springs drive
// the rect (y lags x, so it arcs up out of the taskbar; width and height each
// overshoot once), the tile's colour hands off to the window's, and the real
// titlebar and content fade in at full size, clipped to the growing rect, so
// nothing is resampled. (satoru)
void WindowManager::RenderOpenMotion(Window* win, int t) {
    const int fx = win->x, fy = win->y, fw = win->w, fh = win->h;
    if (win->open_ox < 0) {
        int cx, cy, tw, th;
        wm_tile_target(win, screen_height, cx, cy, tw, th);
        win->open_ox   = (short)cx;
        win->open_oy   = (short)cy;
        win->open_tint = Taskbar::WindowTileColor(win->title);
    }
    const int ts = t - 40;                     // the tile dips first
    int qx = WMMotion::Spring(WMMotion::SPRING_OPEN_X, ts, 620);
    int qy = WMMotion::Spring(WMMotion::SPRING_OPEN_Y, ts, 620);
    int qw = WMMotion::Spring(WMMotion::SPRING_OPEN_W, ts, 620);
    int qh = WMMotion::Spring(WMMotion::SPRING_OPEN_H, ts, 620);
    int cw = Lerp(28, fw, qw), ch = Lerp(24, fh, qh);
    if (cw < 4) cw = 4;
    if (ch < 4) ch = 4;
    int rx = Lerp(win->open_ox, fx + fw / 2, qx) - cw / 2;
    int ry = Lerp(win->open_oy, fy + fh / 2, qy) - ch / 2;
    int rad = Lerp(6, WM_CORNER_RADIUS, Bezier(0.0f, 0.0f, 0.58f, 1.0f, ts, 220));
    int tint_q = Bezier(0.0f, 0.0f, 0.58f, 1.0f, t - 50, 170);
    uint32_t body  = WMMotion::MixColor(win->open_tint, win->bg_color, tint_q);
    uint32_t bar   = WMMotion::MixColor(win->open_tint, COL_TITLE_BG, tint_q);
    uint32_t edge  = WMMotion::MixColor(win->open_tint, win->focused ? wm_get_accent() : COL_BORDER, tint_q);

    // the shadow deepens as the window lifts away from the taskbar.
    int lift = Bezier(0.0f, 0.0f, 0.58f, 1.0f, t - 60, 420);
    WMMotion::FillRoundAlpha(rx - 6, ry + 4, cw + 12, ch + 14, rad + 6, 0xFF000000u, 10 + ((30 * lift) >> 12));
    WMMotion::FillRoundAlpha(rx - 2, ry + 2, cw + 4, ch + 6, rad + 2, 0xFF000000u, 16 + ((40 * lift) >> 12));

    Graphics::FillRoundedRect(rx, ry, cw, ch, rad, body);
    if (win->has_titlebar && ch > WM_TITLEBAR_H + rad) {
        Graphics::FillRoundedRect(rx, ry, cw, WM_TITLEBAR_H + rad, rad, bar);
        Graphics::FillRect(rx, ry + WM_TITLEBAR_H, cw, rad, body);
    }

    // real titlebar + content, fixed at their final place, revealed by the
    // growing rect and faded up from the surface colour.
    int fade = Bezier(0.0f, 0.0f, 0.58f, 1.0f, t - 200, 180);
    if (fade > 0) {
        int veil = 255 - ((255 * fade) >> 12);
        int ix0 = rx > fx ? rx : fx, iy0 = ry > fy ? ry : fy;
        int ix1 = rx + cw < fx + fw ? rx + cw : fx + fw;
        int iy1 = ry + ch < fy + fh ? ry + ch : fy + fh;
        if (win->has_titlebar) {
            int ty1 = fy + WM_TITLEBAR_H < iy1 ? fy + WM_TITLEBAR_H : iy1;
            if (ix1 > ix0 && ty1 > iy0) {
                Graphics::PushClipRect(ix0, iy0, ix1 - ix0, ty1 - iy0);
                RenderTitlebar(win);
                Graphics::PopClipRect();
                if (veil > 0) Graphics::FillRectAlpha(ix0, iy0, ix1 - ix0, ty1 - iy0, (uint8_t)veil, COL_TITLE_BG);
            }
        }
        if (win->render && win->content_w > 0 && win->content_h > 0) {
            int cx0 = win->content_x > ix0 ? win->content_x : ix0;
            int cy0 = win->content_y > iy0 ? win->content_y : iy0;
            int cx1 = win->content_x + win->content_w < ix1 ? win->content_x + win->content_w : ix1;
            int cy1 = win->content_y + win->content_h < iy1 ? win->content_y + win->content_h : iy1;
            if (cx1 > cx0 && cy1 > cy0) {
                Graphics::PushClipRect(cx0, cy0, cx1 - cx0, cy1 - cy0);
                { TaskMem::WindowScope _tms(win->id); win->render(win, win->content_x, win->content_y, win->content_w, win->content_h); }
                Graphics::PopClipRect();
                if (veil > 0) Graphics::FillRectAlpha(cx0, cy0, cx1 - cx0, cy1 - cy0, (uint8_t)veil, win->bg_color);
            }
        }
    }
    Graphics::DrawRect(rx, ry, cw, ch, edge);

    int tcx, tcy, tw, th;
    wm_tile_target(win, screen_height, tcx, tcy, tw, th);
    wm_damage_union(fx, fy, fw, fh, rx, ry, cw, ch);
    wm_damage_union(tcx - tw, tcy - th, tw * 2, th * 2, rx, ry, cw, ch);
}

// close: fold, then let go. a 1.2% inhale, the body folds up into its own
// titlebar while the content slides up and out, then the titlebar contracts
// to a pill and sinks away. drawn from the close-time snapshot. (satoru)
void WindowManager::RenderCloseMotion(Window* win, int t) {
    WMMotion::Snapshot snap = wm_snap(win);
    const int w = snap.w, h = snap.h;
    const int TB = win->has_titlebar ? WM_TITLEBAR_H : 0;
    const int cx = win->x + w / 2, cy = win->y + h / 2;

    int s = 4096;                                           // inhale scale, Q12
    if (t < 70) s += (49 * Bezier(0.2f, 0.8f, 0.3f, 1.0f, t, 70)) >> 12;
    else        s += 49 - ((49 * Bezier(0.6f, 0.0f, 0.4f, 1.0f, t - 70, 150)) >> 12);
    int fold   = Bezier(0.7f, 0.0f, 0.25f, 1.0f, t - 60, 160);
    int exit_q = Bezier(0.5f, 0.0f, 0.75f, 0.0f, t - 60, 110);
    int pill_q = Bezier(0.5f, 0.0f, 0.75f, 0.0f, t - 200, 140);

    int ws  = (w * s) >> 12;
    int xs  = cx - ws / 2;
    int ys  = cy - ((h * s) >> 13);
    int fh  = Lerp(h, TB, fold);
    int fhs = (fh * s) >> 12;
    int tbs = (TB * s) >> 12;

    // the shadow swells with the inhale, then collapses with the fold.
    int shade = t < 70 ? 40 + ((20 * Bezier(0.2f, 0.8f, 0.3f, 1.0f, t, 70)) >> 12)
                       : 60 - ((56 * Bezier(0.0f, 0.0f, 1.0f, 1.0f, t - 70, 170)) >> 12);
    if (shade > 0 && t < 220)
        WMMotion::FillRoundAlpha(xs - 6, ys + 6, ws + 12, fhs + 12, WM_CORNER_RADIUS + 6, 0xFF000000u, shade);

    if (t < 200 || TB == 0) {
        Graphics::FillRoundedRect(xs, ys, ws, fhs, WM_CORNER_RADIUS, win->bg_color);
        if (TB > 0) WMMotion::Blit(snap, 0, 0, w, TB, xs, ys, ws, tbs, 255);
        int u = (26 * exit_q) >> 12;
        int src_y = TB + u;
        int vis = fh - TB;
        if (vis > h - src_y) vis = h - src_y;
        int ca = 255 - ((255 * exit_q) >> 12);
        if (vis > 0 && ca > 0)
            WMMotion::Blit(snap, 0, src_y, w, vis, xs, ys + tbs, ws, (vis * s) >> 12, ca);
    } else {
        if (fhs > tbs) Graphics::FillRect(xs, ys + tbs, ws, fhs - tbs, win->bg_color);
        int pw = Lerp(ws, 96, pill_q);
        int px = cx - pw / 2;
        int py = ys + ((10 * pill_q) >> 12);
        int pr = Lerp(WM_CORNER_RADIUS, tbs / 2, pill_q);
        int pa = 255 - ((255 * pill_q) >> 12);
        WMMotion::FillRoundAlpha(px, py, pw, tbs, pr, COL_TITLE_BG, pa);
        int text_a = 255 - ((255 * Bezier(0.0f, 0.0f, 1.0f, 1.0f, t - 200, 60)) >> 12);
        if (text_a > pa) text_a = pa;
        if (text_a > 0) WMMotion::Blit(snap, 0, 0, w, TB, px, py, pw, tbs, text_a);
    }
    wm_damage(win->x - WM_SHADOW_SIZE - 12, win->y - WM_SHADOW_SIZE - 12,
              w + 2 * (WM_SHADOW_SIZE + 12), h + 2 * (WM_SHADOW_SIZE + 12));
}

// minimize pours the window into its taskbar tile; unminimize (rising) plays
// the same row table backwards. the snapshot is cut into 6 px rows; each row
// falls on one curve while narrowing toward the tile on a faster one, and the
// rows are staggered, which bends the sides into a funnel. rising rows travel
// on the open y-spring, so each lands with a slight overshoot. staggered rows
// drift apart as they fall, so each row is stretched down to meet the next:
// the window stays one continuous ribbon instead of separate slats. (satoru)
void WindowManager::RenderPourMotion(Window* win, int t, bool rising) {
    WMMotion::Snapshot snap = wm_snap(win);
    const int w = snap.w, h = snap.h;
    const int SH = 6;
    const int MAX_ROWS = 256;
    int n = (h + SH - 1) / SH;
    if (n > MAX_ROWS) n = MAX_ROWS;
    if (n < 2) n = 2;
    int rows_sh = (h + n - 1) / n;               // source rows per strip
    int tcx, tcy, tw, th;
    wm_tile_target(win, screen_height, tcx, tcy, tw, th);
    const int scx = win->x + w / 2;
    const int fy_q12 = (th * 4096) / (h > 0 ? h : 1);       // final row squash

    static int s_cx[MAX_ROWS], s_top[MAX_ROWS], s_w[MAX_ROWS], s_h[MAX_ROWS], s_a[MAX_ROWS];
    for (int i = 0; i < n; i++) {
        s_a[i] = -1;
        int sy0 = i * rows_sh;
        if (sy0 >= h) break;
        int shi = h - sy0 < rows_sh ? h - sy0 : rows_sh;
        int scy = win->y + sy0 + shi / 2;
        int d = rising ? (i * 198) / (n - 1) : ((n - 1 - i) * 198) / (n - 1);
        int dcx, dcy, dw, dh_q12, alpha;
        if (!rising) {
            int pi = t - d;
            if (pi < 0) pi = 0;
            if (pi > 360) pi = 360;
            int fq = Bezier(0.7f, 0.0f, 0.35f, 1.0f, pi, 360);
            int nq = Bezier(0.3f, 0.0f, 0.15f, 1.0f, pi, 360);
            dcx = Lerp(scx, tcx, nq);
            dcy = Lerp(scy, tcy, fq);
            dw  = Lerp(w, tw, nq);
            dh_q12 = shi * Lerp(4096, fy_q12, fq);
            alpha = pi < 295 ? 255 : (255 * (360 - pi)) / 65;
        } else {
            int pi = t - 80 - d;
            if (pi < 0) continue;
            int yq = WMMotion::Spring(WMMotion::SPRING_OPEN_Y, pi, 620);
            int wq = Bezier(0.85f, 0.0f, 0.7f, 1.0f, pi, 360);
            dcy = Lerp(tcy, scy, yq);
            dcx = Lerp(tcx, scx, wq);
            dw  = Lerp(tw, w, wq);
            dh_q12 = shi * Lerp(fy_q12, 4096, yq);
            alpha = pi < 65 ? (255 * pi) / 65 : 255;
        }
        int dh = (dh_q12 + 2048) >> 12;
        if (dh < 1) dh = 1;
        if (dw < 1) dw = 1;
        s_cx[i] = dcx; s_top[i] = dcy - dh / 2; s_w[i] = dw; s_h[i] = dh; s_a[i] = alpha;
    }
    for (int i = 0; i < n; i++) {
        if (s_a[i] < 0) continue;
        int sy0 = i * rows_sh;
        int shi = h - sy0 < rows_sh ? h - sy0 : rows_sh;
        int dh = s_h[i];
        if (i + 1 < n && s_a[i + 1] >= 0) {
            int gap = s_top[i + 1] - s_top[i];
            if (gap > dh) dh = gap;
        }
        WMMotion::Blit(snap, 0, sy0, w, shi, s_cx[i] - s_w[i] / 2, s_top[i], s_w[i], dh, s_a[i]);
    }
    wm_damage_union(win->x, win->y, w, h, tcx - tw, tcy - th, tw * 2, th * 2);
}

void WindowManager::RenderMotionOverlay() {
    unsigned int now = Timer::GetRealMs();
    if (wm_spring) {
        wm_fx_c0 = wm_rdtsc();
        for (int i = 0; i < WM_MAX_WINDOWS; i++) {
            Window* w = &windows[i];
            if (w->state == WIN_CLOSED || !w->snap_px) continue;
            if (w->fx_mode != FX_MIN && w->fx_mode != FX_UNMIN) continue;
            int tcx, tcy, tw, th;
            wm_tile_target(w, screen_height, tcx, tcy, tw, th);
            float p = w->fx_gn.x;
            if (p < 0.0f) p = 0.0f;
            if (p > 1.0f) p = 1.0f;
            WMMotion::Snapshot snap = wm_snap(w);
            // the window keeps a faint shadow while it is still mostly itself (satoru)
            if (p < 0.35f && comp_shadow_enabled) {
                float k = 1.0f - p / 0.35f;
                WMMotion::SoftShadow((float)w->x, (float)w->y, (float)w->w, (float)w->h,
                                     (float)WM_CORNER_RADIUS + 6.0f, 30.0f, 12.0f, (int)(90.0f * k * k), false);
            }
            WMMotion::Genie(snap, w->x, w->y, w->w, w->h,
                            (float)(tcx - tw / 2), (float)(tcy - th / 2), (float)tw, (float)th, p);
            int gx0, gy0, gx1, gy1;
            WMMotion::LastGenieBounds(gx0, gy0, gx1, gy1);
            if (p < 0.35f) {
                if (gx0 > w->x - WM_SHADOW_SIZE) gx0 = w->x - WM_SHADOW_SIZE;
                if (gy0 > w->y - WM_SHADOW_SIZE) gy0 = w->y - WM_SHADOW_SIZE;
                if (gx1 < w->x + w->w + WM_SHADOW_SIZE) gx1 = w->x + w->w + WM_SHADOW_SIZE;
                if (gy1 < w->y + w->h + WM_SHADOW_SIZE) gy1 = w->y + w->h + WM_SHADOW_SIZE;
            }
            wm_fx_damage(w, gx0, gy0, gx1, gy1);
            // the dock tile region too, so its catch is repainted (satoru)
            wm_damage(tcx - tw, tcy - th, tw * 2, th * 2);
        }
        wm_fx_probe_end(wm_fx_render_c);
        return;
    }
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        Window* w = &windows[i];
        if (w->state == WIN_CLOSED || !w->snap_px) continue;
        if (w->anim_kind != 3 && w->anim_kind != 5) continue;
        if ((now - w->anim_start_ms) >= w->anim_duration_ms) continue;
        RenderPourMotion(w, wm_motion_t(w, now), w->anim_kind == 5);
    }
}

// shared soft shadow for a window drawn at a float rect, elevation 0..1. (satoru)
static void wm_fx_shadow(const Window* w, float x, float y, float ww, float hh, float rad, float alpha, bool hole) {
    if (!comp_shadow_enabled || w->state == WIN_FULLSCREEN || alpha <= 0.01f) return;
    float e = w->fx_el.x;
    if (e < 0.0f) e = 0.0f;
    if (e > 1.0f) e = 1.0f;
    float k = (float)comp_shadow_opacity_pct / 60.0f * alpha;
    WMMotion::SoftShadow(x, y, ww, hh, rad + 6.0f, 22.0f + 16.0f * e, 8.0f + 8.0f * e,
                         (int)((70.0f + 40.0f * e) * k), hole);
    WMMotion::SoftShadow(x, y, ww, hh, rad, 3.0f + 2.0f * e, 1.0f + 1.0f * e,
                         (int)((60.0f + 20.0f * e) * k), hole);
}

bool WindowManager::FxCapture(Window* win, int x, int y, int w, int h, bool titlebar,
                              unsigned int** px, int* pw, int* ph) {
    if (!win || w <= 0 || h <= 0) return false;
    unsigned long long cap_c0 = wm_rdtsc();
    // a picture of the other side of a full screen toggle: a titlebar means a
    // normal (rounded) window, none at full output size means full screen. (satoru)
    const WindowState sst = win->state;
    if (sst == WIN_FULLSCREEN && titlebar) win->state = WIN_NORMAL;
    else if (sst != WIN_FULLSCREEN && !titlebar && w >= screen_width && h >= screen_height) win->state = WIN_FULLSCREEN;
    // draw the window at the requested geometry over black, copy it out with
    // real corner coverage, then put back what was under it. (satoru)
    const int sx = win->x, sy = win->y, sw = win->w, sh = win->h;
    const bool stb = win->has_titlebar;
    win->x = x; win->y = y; win->w = w; win->h = h; win->has_titlebar = titlebar;
    UpdateContentArea(win);
    bool ok = false;
    if (WMMotion::SaveUnder(x, y, w, h)) {
        Graphics::FillRect(x, y, w, h, 0xFF000000u);
        bool saved_fast = wm_in_fast_render;
        wm_in_fast_render = true;           // no drag-backdrop invalidation from inside (satoru)
        RenderWindowBody(win, false);
        wm_in_fast_render = saved_fast;
        WMMotion::Snapshot s; s.px = (uint32_t*)*px; s.w = *pw; s.h = *ph;
        ok = WMMotion::CapturePremul(s, x, y, w, h, wm_radius(win));
        *px = (unsigned int*)s.px; *pw = s.w; *ph = s.h;
        WMMotion::RestoreUnder();
    }
    win->x = sx; win->y = sy; win->w = sw; win->h = sh; win->has_titlebar = stb;
    win->state = sst;
    UpdateContentArea(win);
    wm_fx_cap_c += wm_rdtsc() - cap_c0;
    return ok;
}

void WindowManager::RenderFxWindow(Window* win) {
    bool old_only = false;
    if (win->fx_mode == FX_MORPH && win->fx_need_old) {
        FxCapture(win, win->fx_ox, win->fx_oy, win->fx_ow, win->fx_oh, win->fx_o_tb,
                  &win->fx_old_px, &win->fx_old_w, &win->fx_old_h);
        win->fx_need_old = false;
        // spread the two captures over two frames: this one shows the old
        // picture only (the springs have barely moved yet) (satoru)
        old_only = win->fx_old_px != nullptr;
    }
    if (!old_only && (win->fx_cap || !win->snap_px)) {
        FxCapture(win, win->x, win->y, win->w, win->h, win->has_titlebar,
                  &win->snap_px, &win->snap_w, &win->snap_h);
        win->fx_cap = false;
    }
    if (!win->snap_px && !old_only) { RenderWindowBody(win, true); return; }
    float rx, ry, rw, rh;
    wm_fx_rect(win, rx, ry, rw, rh);
    float sc = win->fx_sc.x;
    float op = win->fx_op.x;
    if (op < 0.0f) op = 0.0f;
    if (op > 1.0f) op = 1.0f;
    float vw = rw * sc, vh = rh * sc;
    float vx = rx + (rw - vw) * 0.5f, vy = ry + (rh - vh) * 0.5f;
    if (win->fx_mode == FX_OPEN)  vy += (1.0f - sc) * 60.0f;   // rises into place (satoru)
    if (win->fx_mode == FX_CLOSE) vy += (1.0f - sc) * 40.0f;   // sinks a touch as it goes (satoru)
    float rad = (float)wm_radius(win) * sc;
    // morph: the old picture crossfades out early in the travel (satoru)
    int old_a = 0;
    if (win->fx_mode == FX_MORPH && win->fx_old_px) {
        float d = 0.0f;
        for (int k = 0; k < 4; k++) { float e = win->fx_r[k].x - win->fx_r[k].t; d += e < 0 ? -e : e; }
        float k = 1.0f - d / win->fx_d0;
        if (k < 0.0f) k = 0.0f;
        // quick handover early in the travel, while the motion hides it (satoru)
        float q = (k - 0.08f) / 0.30f;
        if (q < 0.0f) q = 0.0f;
        if (q > 1.0f) q = 1.0f;
        q = q * q * (3.0f - 2.0f * q);
        old_a = (int)(256.0f * (1.0f - q));
    }
    wm_fx_shadow(win, vx, vy, vw, vh, rad, op * op, op >= 0.999f);
    if (old_only) old_a = 256;
    WMMotion::Snapshot snap = wm_snap(win);
    if (!old_only) WMMotion::BlitSmooth(snap, vx, vy, vw, vh, (int)(op * 256.0f));
    if (old_a > 0) {
        WMMotion::Snapshot os; os.px = (uint32_t*)win->fx_old_px; os.w = win->fx_old_w; os.h = win->fx_old_h;
        WMMotion::BlitSmooth(os, vx, vy, vw, vh, (old_a * (int)(op * 256.0f)) >> 8);
    }
    const int m = WM_SHADOW_SIZE;
    wm_fx_damage(win, (int)vx - m, (int)vy - m, (int)(vx + vw) + m + 1, (int)(vy + vh) + m + 1);
}

bool WindowManager::TaskbarCatch(int win_id, int tx, int ty, int tw, int th,
                                 int& sx_q8, int& sy_q8, int& flash) {
    Window* w = GetWindow(win_id);
    if (!w || !w->catch_kind) return false;
    unsigned int now = Timer::GetRealMs();
    int el = (int)(now - w->catch_ms);
    if (el < 0) return false;                      // the rows have not arrived yet
    int t = (el * 256) / wm_speed_q8();
    flash = 0;
    if (w->catch_kind == 1) {
        // minimize landing: squash wide and short, then spring back.
        if (t < 40) {
            int q = Bezier(0.0f, 0.0f, 0.58f, 1.0f, t, 40);
            sx_q8 = Lerp(256, 287, q); sy_q8 = Lerp(256, 215, q);
        } else if (t < 460) {
            int q = WMMotion::Spring(WMMotion::SPRING_ICON, t - 40, 420);
            sx_q8 = Lerp(287, 256, q); sy_q8 = Lerp(215, 256, q);
        } else { w->catch_kind = 0; return false; }
        if (t >= 20 && t < 260) flash = (115 * (260 - t)) / 240;
    } else {
        // restore press: dip, then spring back past rest.
        int sc;
        if (t < 70) sc = Lerp(256, 220, Bezier(0.0f, 0.0f, 0.58f, 1.0f, t, 70));
        else if (t < 490) sc = Lerp(220, 256, WMMotion::Spring(WMMotion::SPRING_ICON, t - 70, 420));
        else { w->catch_kind = 0; return false; }
        sx_q8 = sy_q8 = sc;
        if (t >= 40 && t < 260) flash = (115 * (260 - t)) / 220;
    }
    wm_damage(tx - 6, ty - 6, tw + 12, th + 12);
    return true;
}

WMAction WindowManager::GetCurrentAction() {
    return current_action;
}

// ─────────────────────────────────────────────────────────────────────
//  Multi-monitor: window<->output tracking + cross-monitor moves. (satoru)
// ─────────────────────────────────────────────────────────────────────

// fetch monitor `idx` rect from DisplayManager; returns false if it has no
// usable geometry. falls back to the primary screen for index 0. (satoru)
static bool wm_monitor_rect(int idx, int* ox, int* oy, int* ow, int* oh) {
    // real outputs only (see outputs.h); a single screen is the whole desktop. (satoru)
    if (!Outputs::Multi()) {
        if (idx != 0) return false;
        *ox = 0; *oy = 0;
        *ow = WindowManager::GetScreenWidth();
        *oh = WindowManager::GetScreenHeight();
        return true;
    }
    const Output* o = Outputs::Get(idx);
    if (!o || !o->connected) return false;
    *ox = o->x; *oy = o->y; *ow = o->w; *oh = o->h;
    return true;
}
// number of output slots to scan (1 on a single screen); dead ones fail wm_monitor_rect.
static int wm_out_count() { return Outputs::Multi() ? Outputs::Total() : 1; }
// the next live output after `cur`, wrapping; cur itself when there is no other.
static int wm_next_live(int cur) {
    int n = wm_out_count();
    for (int k = 1; k <= n; k++) {
        int c = (cur + k) % n;
        int a, b, w, h;
        if (wm_monitor_rect(c, &a, &b, &w, &h)) return c;
    }
    return cur;
}

void WindowManager::UpdateWindowMonitor(Window* win) {
    if (!win) return;
    int count = wm_out_count();
    if (count <= 1) { win->monitor_id = 0; return; }

    int cx = win->x + win->w / 2;
    int cy = win->y + win->h / 2;
    for (int i = 0; i < count; i++) {
        int ox, oy, ow, oh;
        if (!wm_monitor_rect(i, &ox, &oy, &ow, &oh)) continue;
        if (cx >= ox && cx < ox + ow && cy >= oy && cy < oy + oh) {
            win->monitor_id = i;
            return;
        }
    }
    // center fell outside every monitor - leave the id unchanged.
}

int WindowManager::GetWindowMonitor(int id) {
    Window* win = GetWindow(id);
    return win ? win->monitor_id : 0;
}

void WindowManager::MoveWindowToMonitor(int win_id, int monitor_index) {
    Window* win = GetWindow(win_id);
    if (!win) return;
    int count = wm_out_count();
    if (count <= 1) return;                       // nowhere else to go
    if (monitor_index < 0 || monitor_index >= count) return;
    if (monitor_index == win->monitor_id) return;

    int dox, doy, dow, doh;                        // destination monitor work area
    {
        int fx, fy, fw, fh;
        if (!wm_monitor_rect(monitor_index, &fx, &fy, &fw, &fh)) return;
        wm_area_of(monitor_index, &dox, &doy, &dow, &doh);   // keep clear of the menu bar / dock (satoru)
    }

    // preserve the window's relative offset within its current monitor so it
    // lands in roughly the same spot on the destination. (satoru)
    int sox, soy, sow, soh;
    int rel_x = 0, rel_y = 0;
    if (wm_monitor_rect(win->monitor_id, &sox, &soy, &sow, &soh)) {
        rel_x = win->x - sox;
        rel_y = win->y - soy;
    }

    wm_damage_window(win);

    // maximized windows refill the destination monitor; normal windows keep
    // their size but get clamped to fit the destination rect. (satoru)
    if (win->state == WIN_MAXIMIZED) {
        win->x = dox; win->y = doy;
        win->w = dow; win->h = doh;
    } else {
        if (win->w > dow) win->w = dow;
        if (win->h > doh) win->h = doh;
        int nx = dox + rel_x;
        int ny = doy + rel_y;
        if (nx < dox) nx = dox;
        if (ny < doy) ny = doy;
        if (nx + win->w > dox + dow) nx = dox + dow - win->w;
        if (ny + win->h > doy + doh) ny = doy + doh - win->h;
        win->x = nx; win->y = ny;
    }

    win->monitor_id = monitor_index;
    win->dirty = true;
    UpdateContentArea(win);
    wm_damage(win->x - WM_SHADOW_SIZE, win->y - WM_SHADOW_SIZE,
              win->w + 2*WM_SHADOW_SIZE, win->h + 2*WM_SHADOW_SIZE);
    wm_geom_remember(win);
    SerialLogger::Log("WindowManager: moved window to another monitor\r\n");
}

void WindowManager::SetScreenSize(int w, int h) {
    if (w > 0) screen_width = w;
    if (h > 0) screen_height = h;
    InvalidateDragBackdrop();
}

void WindowManager::ClampAllToDesktop() {
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        Window* win = &windows[i];
        if (win->state == WIN_CLOSED) continue;
        int m = win->monitor_id;
        if (!Outputs::Multi() || m < 0 || m >= OUTPUT_MAX || !Outputs::IsLive(m)) m = 0;
        int ax, ay, aw, ah;
        wm_area_of(m, &ax, &ay, &aw, &ah);
        wm_damage_window(win);
        if (win->state == WIN_MAXIMIZED) {
            win->x = ax; win->y = ay; win->w = aw; win->h = ah;
        } else {
            if (win->w > aw) win->w = aw;
            if (win->h > ah) win->h = ah;
            if (win->x + win->w > ax + aw) win->x = ax + aw - win->w;
            if (win->y + win->h > ay + ah) win->y = ay + ah - win->h;
            if (win->x < ax) win->x = ax;
            if (win->y < ay) win->y = ay;
        }
        win->dirty = true;
        UpdateContentArea(win);
        wm_damage_window(win);
    }
    InvalidateDragBackdrop();
}

void WindowManager::RelocateForOutputs(const int* old_x, const int* old_y, int swap_a, int swap_b) {
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        Window* win = &windows[i];
        if (win->state == WIN_CLOSED) continue;
        int m = win->monitor_id;
        if (m < 0 || m >= OUTPUT_MAX) m = 0;
        // slots exchanged identity: the window stays on the same physical screen
        if (swap_a >= 0 && swap_b >= 0) {
            if (m == swap_a) m = swap_b; else if (m == swap_b) m = swap_a;
        }
        const Output* o = Outputs::Get(m);
        wm_damage_window(win);
        if (o && o->connected) {
            // old origin belongs to the pre-swap slot
            int os = m;
            if (swap_a >= 0 && swap_b >= 0) {
                if (m == swap_a) os = swap_b; else if (m == swap_b) os = swap_a;
            }
            win->x += o->x - old_x[os];
            win->y += o->y - old_y[os];
            win->monitor_id = m;
        } else {
            // its output was switched off: land on the primary work area
            int ax, ay, aw, ah;
            wm_area_of(0, &ax, &ay, &aw, &ah);
            if (win->w > aw) win->w = aw;
            if (win->h > ah) win->h = ah;
            if (win->state == WIN_MAXIMIZED) { win->x = ax; win->y = ay; win->w = aw; win->h = ah; }
            else {
                win->x = ax + (aw - win->w) / 2;
                win->y = ay + (ah - win->h) / 2;
            }
            win->monitor_id = 0;
        }
        win->dirty = true;
        UpdateContentArea(win);
        wm_damage_window(win);
    }
}

// ─────────────────────────────────────────────────────────────────────
//  Window context menu (right-click on the titlebar). (satoru)
// ─────────────────────────────────────────────────────────────────────

bool WindowManager::IsContextMenuOpen() { return wm_ctx_open; }

void WindowManager::CloseContextMenu() {
    if (!wm_ctx_open) return;
    wm_ctx_open = false;
    // damage the popup footprint so it gets painted over next frame.
    wm_damage(wm_ctx_x, wm_ctx_y, WM_CTX_W, WM_CTX_ITEMS * WM_CTX_ITEM_H + 8);
    wm_ctx_win_id = -1;
}

bool WindowManager::HandleRightClick(int mx, int my) {
    // a right-click anywhere dismisses an already-open menu first. (satoru)
    if (wm_ctx_open) { CloseContextMenu(); return true; }

    int top_id = TopWindowAt(mx, my);
    if (top_id <= 0) return false;
    Window* win = GetWindow(top_id);
    if (!win || !win->has_titlebar) return false;

    // only open on the titlebar strip, not the content area. (satoru)
    if (!(my >= win->y && my < win->y + WM_TITLEBAR_H &&
          mx >= win->x && mx < win->x + win->w)) {
        return false;
    }

    Focus(top_id);
    wm_ctx_win_id = top_id;
    wm_ctx_x = mx;
    wm_ctx_y = my;
    // clamp so the popup stays on-screen.
    int menu_h = WM_CTX_ITEMS * WM_CTX_ITEM_H + 8;
    if (wm_ctx_x + WM_CTX_W > screen_width)  wm_ctx_x = screen_width - WM_CTX_W;
    if (wm_ctx_y + menu_h   > screen_height) wm_ctx_y = screen_height - menu_h;
    if (wm_ctx_x < 0) wm_ctx_x = 0;
    if (wm_ctx_y < 0) wm_ctx_y = 0;
    wm_ctx_open = true;
    wm_ctx_open_ms = Timer::GetRealMs();   // start the fade+unfold (satoru)
    wm_damage(wm_ctx_x, wm_ctx_y, WM_CTX_W, menu_h);
    return true;
}

bool WindowManager::HandleContextMenuClick(int mx, int my) {
    if (!wm_ctx_open) return false;

    int menu_h = WM_CTX_ITEMS * WM_CTX_ITEM_H + 8;
    bool inside = (mx >= wm_ctx_x && mx < wm_ctx_x + WM_CTX_W &&
                   my >= wm_ctx_y && my < wm_ctx_y + menu_h);
    if (!inside) { CloseContextMenu(); return true; }   // click-away closes

    int item = (my - (wm_ctx_y + 4)) / WM_CTX_ITEM_H;
    int target = wm_ctx_win_id;
    CloseContextMenu();

    if (item == 1 && target > 0) { ToggleFullscreen(target); return true; }
    if (item == 2 && target > 0) { ToggleMaximize(target); return true; }
    if (item == 3 && target > 0) { Minimize(target); return true; }
    if (item == 4 && target > 0) { UserCloseWindow(target); return true; }
    if (item == 0 && target > 0) {
        // "Move to other monitor": pick the next output after the window's
        // current one, wrapping. with two monitors this is the other one. (satoru)
        int count = wm_out_count();
        if (count >= 2) {
            int cur = GetWindowMonitor(target);
            int next = wm_next_live(cur);
            MoveWindowToMonitor(target, next);
        }
    }
    return true;
}

void WindowManager::RenderContextMenu() {
    if (!wm_ctx_open) return;
    // if the target window vanished, drop the menu. (satoru)
    if (!GetWindow(wm_ctx_win_id)) { wm_ctx_open = false; return; }

    int menu_h = WM_CTX_ITEMS * WM_CTX_ITEM_H + 8;
    // fade+unfold from the click anchor, matching the desktop context menu so all
    // menus share one feel. eased ease-out-cubic off elapsed ms. (satoru)
    unsigned int e = Timer::GetRealMs() - wm_ctx_open_ms;
    float t = (e >= KSS::Motion::Window) ? 1.0f
                                         : Animation::EaseMs(e, KSS::Motion::Window, Animation::EaseOutCubic);
    if (UIStyle::Modern()) {
        // the shared shell menu (same look as the desktop menu) (satoru)
        Window* tw = GetWindow(wm_ctx_win_id);
        const char* items[WM_CTX_ITEMS] = {
            Outputs::Connected() >= 2 ? "Move to Other Display" : "~Move to Other Display",
            tw->state == WIN_FULLSCREEN ? "Exit Full Screen" : "Enter Full Screen",
            tw->state == WIN_MAXIMIZED ? "Restore" : "Maximize",
            "Minimize",
            "!Close",
        };
        int hv = -1;
        if (Mouse::mx >= wm_ctx_x && Mouse::mx < wm_ctx_x + WM_CTX_W && Mouse::my >= wm_ctx_y + 4) {
            hv = (Mouse::my - (wm_ctx_y + 4)) / WM_CTX_ITEM_H;
            if (hv >= WM_CTX_ITEMS) hv = -1;
        }
        MDraw::Menu(wm_ctx_x, wm_ctx_y, WM_CTX_W, 4, WM_CTX_ITEM_H, items, WM_CTX_ITEMS, hv, t);
        wm_damage(wm_ctx_x - 30, wm_ctx_y - 30, WM_CTX_W + 60, menu_h + 70);
        return;
    }
    int draw_h = (int)(menu_h * (0.62f + 0.38f * t) + 0.5f);
    if (draw_h < 6) draw_h = 6;
    // panel background + border (reuse the titlebar palette). (satoru)
    Graphics::FillRoundedRect(wm_ctx_x, wm_ctx_y, WM_CTX_W, draw_h, 6, COL_TITLE_FOCUSED);
    Graphics::DrawRect(wm_ctx_x, wm_ctx_y, WM_CTX_W, draw_h, COL_BORDER);

    int count = Outputs::Connected();
    // entry 0 - "Move to other monitor" (dimmed when only one output). (satoru)
    int iy = wm_ctx_y + 4;
    uint32_t txt = (count >= 2) ? COL_TITLE_TEXT : COL_TITLE_TEXT_DIM;
    Graphics::PushClipRect(wm_ctx_x, wm_ctx_y, WM_CTX_W, draw_h);
    Graphics::DrawString(wm_ctx_x + 12, iy + (WM_CTX_ITEM_H - 12) / 2,
                         "Move to other monitor", txt, 0x00000000);
    Graphics::PopClipRect();
    if (t < 1.0f) {
        uint8_t dim = (uint8_t)((1.0f - t) * 150.0f);
        if (dim > 0) Graphics::FillRectAlpha(wm_ctx_x, wm_ctx_y, WM_CTX_W, draw_h, dim, 0xFF000000u);
        wm_damage(wm_ctx_x, wm_ctx_y, WM_CTX_W, menu_h);   // keep repainting while it fades (satoru)
    }
}

int WindowManager::CreateWindow(const char* title, int x, int y, int w, int h,
    WindowRenderFunc render_func, WindowInputFunc input_func) {
    Window* win = CreateWindow(title, x, y, w, h);
    if (!win) return -1;
    win->render = render_func;
    win->input = input_func;
    return win->id;
}

// ─────────────────────────────────────────────────────────────────────
//  Compositor-side public API: animation tick, config reload, alpha,
//  taskbar anchor.  See window_manager.h.
// ─────────────────────────────────────────────────────────────────────
// damage the union of what a spring-animated window painted last frame and
// this frame (shadow included), then remember this frame's bounds. (satoru)
static void wm_fx_damage(Window* w, int x0, int y0, int x1, int y1) {
    int ax0 = x0, ay0 = y0, ax1 = x1, ay1 = y1;
    if (w->fx_bx1 > w->fx_bx0 && w->fx_by1 > w->fx_by0) {
        if (w->fx_bx0 < ax0) ax0 = w->fx_bx0;
        if (w->fx_by0 < ay0) ay0 = w->fx_by0;
        if (w->fx_bx1 > ax1) ax1 = w->fx_bx1;
        if (w->fx_by1 > ay1) ay1 = w->fx_by1;
    }
    if (ax1 > ax0 && ay1 > ay0) wm_damage(ax0, ay0, ax1 - ax0, ay1 - ay0);
    w->fx_bx0 = x0; w->fx_by0 = y0; w->fx_bx1 = x1; w->fx_by1 = y1;
}

// spring step for one window; returns after retiring a finished motion. (satoru)
static void wm_fx_tick(Window* w, unsigned long long now, int& focused,
                           int& capture_id, int& action_id, WMAction& action) {
    if (!w->fx_us) w->fx_us = now;
    float dt = (float)(now - w->fx_us) / 1000000.0f;
    if (dt > 0.5f) dt = 0.5f;
    w->fx_us = now;
    w->fx_el.t = w->focused ? 1.0f : 0.0f;
    WMMotion::SpringStep(w->fx_el, wm_resp(0.30f), 1.0f, dt);
    switch (w->fx_mode) {
        case FX_OPEN:
            WMMotion::SpringStep(w->fx_sc, wm_resp(0.36f), 0.80f, dt);
            WMMotion::SpringStep(w->fx_op, wm_resp(0.20f), 1.0f, dt);
            if (WMMotion::SpringRest(w->fx_sc, 0.0015f) && WMMotion::SpringRest(w->fx_op, 0.004f)) {
                WMMotion::SpringSet(w->fx_sc, 1.0f); WMMotion::SpringSet(w->fx_op, 1.0f);
                w->fx_mode = FX_NONE;
                wm_snap_release(w);
                wm_fx_damage(w, w->x - WM_SHADOW_SIZE, w->y - WM_SHADOW_SIZE,
                             w->x + w->w + WM_SHADOW_SIZE, w->y + w->h + WM_SHADOW_SIZE);
                w->fx_bx0 = w->fx_bx1 = 0;
            }
            break;
        case FX_CLOSE:
            WMMotion::SpringStep(w->fx_sc, wm_resp(0.30f), 1.0f, dt);
            WMMotion::SpringStep(w->fx_op, wm_resp(0.19f), 1.0f, dt);
            if (w->fx_rect)   // a morph in flight keeps travelling while it fades (satoru)
                for (int k = 0; k < 4; k++) WMMotion::SpringStep(w->fx_r[k], wm_resp(0.42f), 0.86f, dt);
            if (w->fx_op.x < 0.015f) {
                wm_fx_damage(w, 0, 0, 0, 0);
                w->fx_bx0 = w->fx_bx1 = 0;
                wm_damage_window(w);
                wm_snap_release(w);
                wm_fx_old_release(w);
                w->fx_mode = FX_NONE;
                w->fx_rect = false;
                w->anim_kind = 0;
                w->state = WIN_CLOSED;
                w->visible = false;
                w->had_last = false;
                if (w->on_close) { void (*cb)(Window*) = w->on_close; w->on_close = nullptr; cb(w); }
                if (focused == w->id) focused = -1;
                if (capture_id == w->id) capture_id = -1;
                if (action_id == w->id) { action_id = -1; action = WM_NONE; }
            }
            break;
        case FX_MIN:
            WMMotion::SpringStep(w->fx_gn, wm_resp(0.56f), 1.0f, dt);
            if (w->fx_gn.x > 0.86f && !w->fx_caught) {
                // the dock tile takes the landing (squash + flash) (satoru)
                w->fx_caught = true;
                w->catch_kind = 1;
                w->catch_ms = Timer::GetRealMs();
            }
            if (w->fx_gn.x > 0.985f) {
                WMMotion::SpringSet(w->fx_gn, 1.0f);
                wm_fx_damage(w, 0, 0, 0, 0);
                w->fx_bx0 = w->fx_bx1 = 0;
                wm_damage_window(w);
                w->fx_mode = FX_NONE;
                w->anim_kind = 0;
                w->state = WIN_MINIMIZED;
                w->visible = false;
                w->had_last = false;
            }
            break;
        case FX_UNMIN:
            WMMotion::SpringStep(w->fx_gn, wm_resp(0.50f), 1.0f, dt);
            if (w->fx_gn.x < 0.004f && WMMotion::SpringRest(w->fx_gn, 0.004f)) {
                WMMotion::SpringSet(w->fx_gn, 0.0f);
                wm_fx_damage(w, 0, 0, 0, 0);
                w->fx_bx0 = w->fx_bx1 = 0;
                wm_damage_window(w);
                wm_snap_release(w);
                w->fx_mode = FX_NONE;
                w->anim_kind = 0;
            }
            break;
        case FX_MORPH: {
            w->fx_r[0].t = (float)w->x; w->fx_r[1].t = (float)w->y;
            w->fx_r[2].t = (float)w->w; w->fx_r[3].t = (float)w->h;
            bool rest = true;
            for (int k = 0; k < 4; k++) {
                WMMotion::SpringStep(w->fx_r[k], wm_resp(0.42f), 0.86f, dt);
                if (!WMMotion::SpringRest(w->fx_r[k], 0.35f)) rest = false;
            }
            if (rest) {
                wm_fx_damage(w, w->x - WM_SHADOW_SIZE, w->y - WM_SHADOW_SIZE,
                             w->x + w->w + WM_SHADOW_SIZE, w->y + w->h + WM_SHADOW_SIZE);
                w->fx_bx0 = w->fx_bx1 = 0;
                w->fx_mode = FX_NONE;
                w->fx_rect = false;
                wm_snap_release(w);
                wm_fx_old_release(w);
            }
            break;
        }
        default: break;
    }
    if (w->fx_mode == FX_NONE && WMMotion::SpringRest(w->fx_el, 0.002f)) {
        w->fx_el.x = w->fx_el.t; w->fx_el.v = 0.0f;
        w->fx_us = 0;   // idle: the next motion starts its clock fresh (satoru)
    } else if (w->fx_mode == FX_NONE && w->visible && w->state != WIN_MINIMIZED) {
        // the focus elevation is still easing: keep its shadow band painted (satoru)
        wm_damage(w->x - WM_SHADOW_SIZE, w->y - WM_SHADOW_SIZE,
                  w->w + 2 * WM_SHADOW_SIZE, w->h + 2 * WM_SHADOW_SIZE);
    }
}

void WindowManager::TickAnimations() {
    wm_geom_flush(Timer::GetRealMs());
    if (wm_spring) {
        unsigned long long now_us = wm_now_us();
        for (int i = 0; i < WM_MAX_WINDOWS; i++) {
            Window* w = &windows[i];
            if (w->state == WIN_CLOSED) continue;
            wm_fx_tick(w, now_us, focused_id, wm_input_capture_id,
                                  action_window_id, current_action);
        }
        return;
    }
    unsigned int now = Timer::GetRealMs();
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        Window* w = &windows[i];
        if (w->state == WIN_CLOSED) continue;
        if (w->anim_kind == 0) continue;
        if (w->anim_duration_ms != 0 &&
            (now - w->anim_start_ms) < w->anim_duration_ms) {
            continue;
        }
        // animation done - finalise according to kind
        unsigned char k = w->anim_kind;
        w->anim_kind = 0;
        switch (k) {
            case 1:  /* open  */ break;          // already visible
            case 2:  /* close */
                wm_damage_window(w);
                wm_snap_release(w);
                w->state = WIN_CLOSED;
                w->visible = false;
                w->had_last = false;
                if (w->on_close) { void (*cb)(Window*) = w->on_close; w->on_close = nullptr; cb(w); }
                if (focused_id == w->id) focused_id = -1;
                if (wm_input_capture_id == w->id) wm_input_capture_id = -1;
                if (action_window_id == w->id) {
                    action_window_id = -1;
                    current_action = WM_NONE;
                }
                break;
            case 3:  /* minimize: keep the snapshot for the restore */
                wm_damage_window(w);
                w->state = WIN_MINIMIZED;
                w->visible = false;
                w->had_last = false;
                break;
            case 4:  /* restore */ break;
            case 5:  /* unminimize */
                wm_damage_window(w);
                wm_snap_release(w);
                break;
            default: break;
        }
    }
}

void WindowManager::ReloadFromConfig() {
    comp_shadow_enabled       = UIConfig::Bool("compositor.shadow_enabled",       true);
    comp_shadow_radius        = UIConfig::Int ("compositor.shadow_radius",        8);
    comp_shadow_opacity_pct   = UIConfig::Int ("compositor.shadow_opacity",       60);
    comp_shadow_during_drag   = UIConfig::Bool("compositor.shadow_during_drag",   false);
    comp_animations_enabled   = UIConfig::Bool("compositor.window_animations",    true);
    comp_anim_duration_ms     = UIConfig::Int ("compositor.animation_speed_ms",   180);
    comp_default_alpha        = UIConfig::Int ("compositor.window_alpha",         255);
    comp_frosted_titlebar     = UIConfig::Bool("compositor.frosted_titlebar",     true);
    comp_reduced_motion       = UIConfig::Bool("compositor.reduced_motion",       false);
    {   // spring (default) or the classic timed motion drafts (satoru)
        const char* m = UIConfig::Str("compositor.motion", "spring");
        wm_spring = !(m && m[0] == 'c');
    }

    // sanity-clamp
    if (comp_shadow_radius      < 0)   comp_shadow_radius = 0;
    if (comp_shadow_radius      > 16)  comp_shadow_radius = 16;
    if (comp_shadow_opacity_pct < 0)   comp_shadow_opacity_pct = 0;
    if (comp_shadow_opacity_pct > 100) comp_shadow_opacity_pct = 100;
    if (comp_anim_duration_ms   < 0)   comp_anim_duration_ms = 0;
    if (comp_anim_duration_ms   > 2000) comp_anim_duration_ms = 2000;
    if (comp_default_alpha      < 0)   comp_default_alpha = 0;
    if (comp_default_alpha      > 255) comp_default_alpha = 255;

    // Display: refresh-rate driven frame pacing.
    // the frame pacer owns the rate now (display.refresh_mode auto/fixed,
    // kurono.hz, the detected panel rate, the adaptive step). the old
    // display.refresh_hz key defaulted to 60 in every ui.conf and silently
    // pinned the desktop to 60 whatever the panel could do. (satoru)
    FramePacer::ReloadConfig();
    int refresh_hz = (int)FramePacer::CapHz();
    // Drag throttle: 1 frame interval - coalesces multi-event-per-frame
    // pointer storms into a single window move so we never repaint twice
    // for the same monitor scan-out.
    wm_drag_min_gap_ms = (unsigned int)(1000 / (refresh_hz > 0 ? refresh_hz : 60));
    if (wm_drag_min_gap_ms < 2)  wm_drag_min_gap_ms = 2;
    if (wm_drag_min_gap_ms > 16) wm_drag_min_gap_ms = 16;
    (void)comp_frosted_titlebar; // currently passive - read by titlebar render
}

void WindowManager::SetAlpha(int id, unsigned char a) {
    Window* w = GetWindow(id);
    if (w) w->alpha = a;
}

unsigned char WindowManager::GetAlpha(int id) {
    Window* w = GetWindow(id);
    return w ? w->alpha : 255;
}

void WindowManager::SetTaskbarAnchor(int id, int x, int y) {
    Window* w = GetWindow(id);
    if (!w) return;
    w->anchor_x = (short)x;
    w->anchor_y = (short)y;
}
