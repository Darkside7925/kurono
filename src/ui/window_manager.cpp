#include "window_manager.h"
#include "../drivers/graphics.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include "../drivers/keyboard.h"
#include "../system/ui_config.h"

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
static int  comp_anim_duration_ms     = 90;
static int  comp_default_alpha        = 255;
static bool comp_frosted_titlebar     = true;
static bool comp_reduced_motion       = false;

// Drag throttle. Updated by ReloadFromConfig() from display.refresh_hz.
// 0 means "no throttle"; otherwise per-frame minimum gap in ms.
static unsigned int wm_drag_min_gap_ms = 6;       // ~165 Hz upper bound
static unsigned int wm_last_drag_ms    = 0;

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

// Damage helper  -  push a rect to Graphics' dirty-region tracker, but
// only the portion that lies on-screen.  Cheap to call.
static inline void wm_damage(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    int sw = WindowManager::GetScreenWidth();
    int sh = WindowManager::GetScreenHeight();
    if (x >= sw || y >= sh) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > sw) w = sw - x;
    if (y + h > sh) h = sh - y;
    if (w <= 0 || h <= 0) return;
    Graphics::MarkDirty(x, y, w, h);
}

// Damage a window's last and current rect, with shadow margin.
static inline void wm_damage_window(const Window* w) {
    if (!w) return;
    const int m = WM_SHADOW_SIZE + 2;
    if (w->had_last) {
        wm_damage(w->last_x - m, w->last_y - m,
                  w->last_w + 2*m, w->last_h + 2*m);
    }
    if (w->state != WIN_CLOSED && w->visible && w->state != WIN_MINIMIZED) {
        wm_damage(w->x - m, w->y - m, w->w + 2*m, w->h + 2*m);
    }
}
static void wm_clamp_drag_bounds(Window* win) {
    if (!win) return;
    int visible_w = win->w < 96 ? win->w : 96;
    int visible_h = win->has_titlebar ? WM_TITLEBAR_H : 24;
    if (visible_h < 16) visible_h = 16;
    int win_w = win->w > 0 ? win->w : 1;
    int win_h = win->h > 0 ? win->h : 1;
    int min_x = wm_desktop_x - (win_w - visible_w);
    int max_x = wm_desktop_x + wm_desktop_w - visible_w;
    int min_y = wm_desktop_y;
    int max_y = wm_desktop_y + wm_desktop_h - visible_h;
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
    int max_w = wm_desktop_x + wm_desktop_w - win->x;
    int max_h = wm_desktop_y + wm_desktop_h - win->y;
    if (max_w < WM_MIN_WIDTH) max_w = WM_MIN_WIDTH;
    if (max_h < WM_MIN_HEIGHT) max_h = WM_MIN_HEIGHT;
    if (win->w < WM_MIN_WIDTH) win->w = WM_MIN_WIDTH;
    if (win->h < WM_MIN_HEIGHT) win->h = WM_MIN_HEIGHT;
    if (win->w > max_w) win->w = max_w;
    if (win->h > max_h) win->h = max_h;
}

// kurono ui color scheme  -  modern glassmorphism
// Static fallbacks used when UIConfig is not yet available.
// At render time, focused colours read theme.accent from UIConfig.
static uint32_t wm_get_accent(){
    return UIConfig::Color("theme.accent", 0xFF6C8CFF);
}
#define COL_TITLE_BG       0xFF1C1C2E  // frosted dark
#define COL_TITLE_FOCUSED  0xFF22223A  // focused: base  -  overlaid with accent
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
        windows[i].had_last = false;
        windows[i].visible = false;
        windows[i].focused = false;
    }
    wm_input_capture_id = -1;
    wm_last_drag_ms = 0;

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
    wmcpy(win->title, title, 64);
    if (w < WM_MIN_WIDTH)  w = WM_MIN_WIDTH;
    if (h < WM_MIN_HEIGHT) h = WM_MIN_HEIGHT;
    if (w > screen_width)   w = screen_width;
    if (h > wm_desktop_h)   h = wm_desktop_h;
    if (x < 0) x = wm_desktop_x + (wm_desktop_w - w) / 2;
    if (y < 0) y = wm_desktop_y + (wm_desktop_h - h) / 2;
    if (x < wm_desktop_x) x = wm_desktop_x;
    if (y < wm_desktop_y) y = wm_desktop_y;
    if (x + w > wm_desktop_x + wm_desktop_w) x = wm_desktop_x + wm_desktop_w - w;
    if (y + h > wm_desktop_y + wm_desktop_h) y = wm_desktop_y + wm_desktop_h - h;
    win->x = x; win->y = y;
    win->w = w; win->h = h;
    win->saved_x = x; win->saved_y = y;
    win->saved_w = w; win->saved_h = h;
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
    win->user_data = nullptr;
    win->dirty = true;

    // compositor: open-from-nothing animation, default alpha
    win->anim_kind        = comp_animations_enabled && !comp_reduced_motion ? 1 : 0;
    win->anim_start_ms    = Timer::GetRealMs();
    win->anim_duration_ms = (unsigned short)comp_anim_duration_ms;
    win->alpha            = (unsigned char)comp_default_alpha;
    win->anchor_x         = (short)(x + w / 2);
    win->anchor_y         = (short)(screen_height - 22);
    win->last_x = x; win->last_y = y;
    win->last_w = w; win->last_h = h;
    win->had_last = false;

    UpdateContentArea(win);
    wm_damage_window(win);

    if (window_count < WM_MAX_WINDOWS) window_count++;
    Focus(win->id);

    return win;
}

void WindowManager::CloseWindow(int id) {
    Window* win = GetWindow(id);
    if (!win) return;
    // Defer the actual transition to CLOSED until the close animation
    // finishes; TickAnimations() will retire the window then.  Skip
    // the defer if animations are off, otherwise the user sees no
    // exit feedback.
    if (comp_animations_enabled && !comp_reduced_motion &&
        comp_anim_duration_ms > 0 && win->anim_kind != 2) {
        win->anim_kind        = 2;            // close
        win->anim_start_ms    = Timer::GetRealMs();
        win->anim_duration_ms = (unsigned short)comp_anim_duration_ms;
        return;
    }
    wm_damage_window(win);
    win->state = WIN_CLOSED;
    win->visible = false;
    win->had_last = false;
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
            windows[i].visible = false;
            windows[i].had_last = false;
            windows[i].anim_kind = 0;
        }
    }
    focused_id = -1;
    wm_input_capture_id = -1;
    action_window_id = -1;
    current_action = WM_NONE;
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
    if (comp_animations_enabled && !comp_reduced_motion &&
        comp_anim_duration_ms > 0 && win->anim_kind != 3) {
        // defer hide until minimize animation completes
        win->anim_kind        = 3;
        win->anim_start_ms    = Timer::GetRealMs();
        win->anim_duration_ms = (unsigned short)comp_anim_duration_ms;
        return;
    }
    wm_damage_window(win);
    win->state = WIN_MINIMIZED;
    win->visible = false;
    win->had_last = false;
}

void WindowManager::Maximize(int id) {
    Window* win = GetWindow(id);
    if (!win) return;
    if (win->state != WIN_MAXIMIZED) {
        win->saved_x = win->x; win->saved_y = win->y;
        win->saved_w = win->w; win->saved_h = win->h;
    }
    wm_damage_window(win);
    win->x = 0; win->y = GetDesktopY();
    win->w = screen_width; win->h = GetDesktopH();
    win->state = WIN_MAXIMIZED;
    win->visible = true;
    win->dirty = true;
    UpdateContentArea(win);
    wm_damage(win->x, win->y, win->w, win->h);
}

void WindowManager::Restore(int id) {
    Window* win = GetWindow(id);
    if (!win) return;
    wm_damage_window(win);
    int rx = win->saved_x, ry = win->saved_y;
    int rw = win->saved_w, rh = win->saved_h;
    if (rw < WM_MIN_WIDTH)  rw = WM_MIN_WIDTH;
    if (rh < WM_MIN_HEIGHT) rh = WM_MIN_HEIGHT;
    if (rw > screen_width)  rw = screen_width;
    if (rh > wm_desktop_h)  rh = wm_desktop_h;
    if (rx < wm_desktop_x) rx = wm_desktop_x;
    if (ry < wm_desktop_y) ry = wm_desktop_y;
    if (rx + rw > wm_desktop_x + wm_desktop_w) rx = wm_desktop_x + wm_desktop_w - rw;
    if (ry + rh > wm_desktop_y + wm_desktop_h) ry = wm_desktop_y + wm_desktop_h - rh;
    win->x = rx; win->y = ry;
    win->w = rw; win->h = rh;
    win->state = WIN_NORMAL;
    win->visible = true;
    win->dirty = true;
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
    if (win->state == WIN_MAXIMIZED) Restore(id);
    else Maximize(id);
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
            wm_damage(windows[i].x, windows[i].y, windows[i].w, WM_TITLEBAR_H + 1);
            wm_damage(windows[i].x, windows[i].y, 1, windows[i].h);
            wm_damage(windows[i].x + windows[i].w - 1, windows[i].y, 1, windows[i].h);
            wm_damage(windows[i].x, windows[i].y + windows[i].h - 1, windows[i].w, 1);
        }
    }
    Window* win = GetWindow(id);
    if (win) {
        win->focused = true;
        win->dirty = true;
        wm_damage_window(win);
        focused_id = id;
        BringToFront(id);
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
}

void WindowManager::MoveWindow(int id, int x, int y) {
    Window* win = GetWindow(id);
    if (!win) return;
    if (win->x == x && win->y == y) return;
    wm_damage_window(win);
    win->x = x; win->y = y;
    win->dirty = true;
    UpdateContentArea(win);
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

void WindowManager::MarkDirty(int id) {
    Window* win = GetWindow(id);
    if (!win) return;
    win->dirty = true;
    wm_damage(win->x, win->y, win->w, win->h);
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
    return px >= win->x && px < win->x + win->w &&
           py >= win->y && py < win->y + win->h;
}

void WindowManager::UpdateContentArea(Window* win) {
    if (win->has_titlebar) {
        win->content_x = win->x + WM_BORDER_W;
        win->content_y = win->y + WM_TITLEBAR_H;
        win->content_w = win->w - WM_BORDER_W * 2;
        win->content_h = win->h - WM_TITLEBAR_H - WM_BORDER_W;
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

    // titlebar drag  -  skipped for fullscreen since drag has no effect
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
    // insertion sort  -  N <= 32, almost-sorted in practice
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
    return true;
}

void WindowManager::Update(int mouse_x, int mouse_y, bool mouse_down, bool mouse_clicked) {
    // handle ongoing drag/resize
    if (current_action != WM_NONE && mouse_down) {
        Window* win = GetWindow(action_window_id);
        if (WM_LIKELY(win != nullptr)) {
            unsigned int now = Timer::GetRealMs();
            bool throttle = (current_action == WM_DRAG) && wm_drag_min_gap_ms > 0
                            && (now - wm_last_drag_ms) < wm_drag_min_gap_ms;
            if (!throttle) {
                int ox = win->x, oy = win->y, ow = win->w, oh = win->h;
                if (wm_apply_action(win, current_action, mouse_x, mouse_y,
                                    drag_offset_x, drag_offset_y)) {
                    wm_damage(ox - WM_SHADOW_SIZE, oy - WM_SHADOW_SIZE,
                              ow + 2*WM_SHADOW_SIZE, oh + 2*WM_SHADOW_SIZE);
                    wm_damage(win->x - WM_SHADOW_SIZE, win->y - WM_SHADOW_SIZE,
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

                // close button
                if (win->closable) {
                    int dx = mouse_x - btn_start_x;
                    int dy = mouse_y - btn_cy;
                    if (dx*dx + dy*dy <= btn_r2) {
                        CloseWindow(top_id);
                        return;
                    }
                }

                // minimize button
                {
                    int dx = mouse_x - (btn_start_x + 22);
                    int dy = mouse_y - btn_cy;
                    if (dx*dx + dy*dy <= btn_r2) {
                        Minimize(top_id);
                        return;
                    }
                }

                // maximize button
                {
                    int dx = mouse_x - (btn_start_x + 44);
                    int dy = mouse_y - btn_cy;
                    if (dx*dx + dy*dy <= btn_r2) {
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
                    win->input(win, 1, local_x, local_y); // event 1 = click
                }
            }
        }
    }

    prev_mouse_down = mouse_down;
}

void WindowManager::RenderShadow(Window* win) {
    if (!comp_shadow_enabled) return;
    if (win->state == WIN_FULLSCREEN || win->state == WIN_MAXIMIZED) return;
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

void WindowManager::RenderTitlebar(Window* win) {
    bool focused = win->focused;
    uint32_t bg       = focused ? COL_TITLE_FOCUSED : COL_TITLE_BG;
    uint32_t text_col = focused ? COL_TITLE_TEXT    : COL_TITLE_TEXT_DIM;
    int wx = win->x, wy = win->y, ww = win->w;

    // titlebar background with rounded top corners
    Graphics::FillRoundedRect(wx, wy, ww, WM_TITLEBAR_H, WM_CORNER_RADIUS, bg);
    Graphics::FillRect(wx, wy + WM_CORNER_RADIUS, ww, WM_TITLEBAR_H - WM_CORNER_RADIUS, bg);

    if (focused) {
        for (int row = 0; row < 4; row++) {
            uint8_t a = (uint8_t)(30 - row * 6);
            if (ww > 2)
                Graphics::FillRectAlpha(wx + 1, wy + WM_TITLEBAR_H - 4 + row,
                                        ww - 2, 1, a, 0xFF000000u);
        }
    }

    Graphics::FillRect(wx, wy + WM_TITLEBAR_H, ww, 1, COL_SEPARATOR);

    int btn_y = wy + WM_TITLEBAR_H / 2;
    const int btn_r = 6;
    int btn_start_x = wx + 14;

    if (win->closable) {
        Graphics::FillCircle(btn_start_x, btn_y, btn_r, COL_CLOSE_BTN);
        if (focused) {
            Graphics::DrawLine(btn_start_x - 2, btn_y - 2, btn_start_x + 2, btn_y + 2, 0xFF401010);
            Graphics::DrawLine(btn_start_x + 2, btn_y - 2, btn_start_x - 2, btn_y + 2, 0xFF401010);
        }
    }

    Graphics::FillCircle(btn_start_x + 22, btn_y, btn_r, COL_MIN_BTN);
    if (focused) {
        Graphics::FillRect(btn_start_x + 19, btn_y, 6, 1, 0xFF403010);
    }

    Graphics::FillCircle(btn_start_x + 44, btn_y, btn_r, COL_MAX_BTN);
    if (focused) {
        Graphics::DrawLine(btn_start_x + 42, btn_y - 2, btn_start_x + 46, btn_y + 2, 0xFF103010);
        Graphics::DrawLine(btn_start_x + 46, btn_y - 2, btn_start_x + 42, btn_y + 2, 0xFF103010);
    }

    int title_area_start = btn_start_x + 62;
    int title_area_end = wx + ww - 10;
    if (title_area_end > title_area_start + 8) {
        int max_chars = (title_area_end - title_area_start) / 8;
        int len = wmlen(win->title);
        if (len > max_chars) len = max_chars;
        int title_w = len * 8;
        int title_x = title_area_start + (title_area_end - title_area_start - title_w) / 2;
        if (title_x < title_area_start) title_x = title_area_start;
        Graphics::SetClipRect(title_area_start, wy,
                              title_area_end - title_area_start, WM_TITLEBAR_H);
        Graphics::DrawString(title_x, wy + (WM_TITLEBAR_H - 12) / 2, win->title, text_col, 0x00000000);
        Graphics::ClearClipRect();
    }

    if (focused && ww > 2 * WM_CORNER_RADIUS) {
        Graphics::FillRect(wx + WM_CORNER_RADIUS, wy, ww - 2 * WM_CORNER_RADIUS, 1, wm_get_accent());
    }
}

// Compute the animation phase (0..256, fixed-point Q8) for a window.
// Returns 256 (= 1.0) if no animation is active or animation has
// completed.  Used by Render() to drive the per-window post-pass.
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

void WindowManager::Render() {
    // First retire any animations that have completed since the last
    // tick so windows in their final CLOSE/MINIMIZE phase are dropped
    // before we build the visibility list.
    TickAnimations();

    // render windows in z-order (lowest first)
    // build sorted index
    int indices[WM_MAX_WINDOWS];
    int count = 0;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        const Window& w = windows[i];
        if (w.state != WIN_CLOSED && w.visible && w.state != WIN_MINIMIZED)
            indices[count++] = i;
    }

    // sort by z-order  -  insertion sort (count small, near-sorted)
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
            // o covers w iff o's rect contains w's rect.
            if (o->x <= w->x && o->y <= w->y &&
                o->x + o->w >= w->x + w->w &&
                o->y + o->h >= w->y + w->h) {
                draw[i] = false;
                break;
            }
        }
    }

    // render each window
    bool dragging = IsDragging();
    unsigned int now = Timer::GetRealMs();
    uint32_t accent = wm_get_accent();
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

        // For minimize/restore we draw a shrinking outline that
        // travels toward the taskbar anchor instead of rendering the
        // full window content.  This is the cheap-but-honest way to
        // give a "fly to taskbar" effect without a real per-window
        // framebuffer.
        if (WM_UNLIKELY(kind == 3 && p_raw < 256)) {
            int q = 256 - p;                       // 256..0
            int ax = win->anchor_x, ay = win->anchor_y;
            int rx = win->x + ((ax - win->x) * (256 - q)) / 256;
            int ry = win->y + ((ay - win->y) * (256 - q)) / 256;
            int rw = (win->w * q) / 256;
            int rh = (win->h * q) / 256;
            if (rw < 4)  rw = 4;
            if (rh < 4)  rh = 4;
            unsigned int outline = 0xFF000000u | (win->focused ? 0x6C8CFFu : 0x303050u);
            Graphics::DrawRect(rx, ry, rw, rh, outline);
            unsigned int fa = (unsigned int)(p_vis / 4);  // 0..64
            Graphics::FillRectAlpha(rx + 1, ry + 1, rw - 2, rh - 2, (uint8_t)fa, 0xFF101020u);
            wm_damage(rx, ry, rw, rh);
            win->dirty = false;
            continue;
        }
        if (WM_UNLIKELY(kind == 4 && p_raw < 256)) {
            int q = p;                             // 0..256
            int ax = win->anchor_x, ay = win->anchor_y;
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

        // shadow  -  skip during drag/resize unless user opted in.
        if (!dragging || comp_shadow_during_drag) {
            RenderShadow(win);
        }

        // window body  -  smooth rounded rectangle
        uint32_t border = win->focused ? accent : COL_BORDER;

        // full body background with rounded corners
        Graphics::FillRoundedRect(win->x, win->y, win->w, win->h, WM_CORNER_RADIUS, win->bg_color);

        // titlebar (draws over the top portion)
        if (win->has_titlebar) RenderTitlebar(win);

        // subtle border
        Graphics::DrawRect(win->x, win->y, win->w, win->h, border);

        // content (clipped to content area so text doesn't bleed outside
        // when the user shrinks the window below the content's natural size)
        if (win->render && win->content_w > 0 && win->content_h > 0) {
            Graphics::SetClipRect(win->content_x, win->content_y,
                                  win->content_w, win->content_h);
            win->render(win, win->content_x, win->content_y, win->content_w, win->content_h);
            Graphics::ClearClipRect();
        }

        // ── post-pass: per-window opacity + open/close fade overlay ──
        int combined = (int)win->alpha * p_vis / 256;
        if (combined < 255) {
            unsigned int dim = (unsigned int)(255 - combined);
            if (kind == 1 && dim > 96) {
                // Never start a new app window fully black. If the frame loop
                // stalls during launch, a full blackout reads as a dead app.
                dim = 96;
            }
            Graphics::FillRectAlpha(win->x, win->y, win->w, win->h, (uint8_t)dim, 0xFF000000u);
        }

        // Animated windows must keep redrawing until phase reaches 1.
        if (kind != 0 && p_raw < 256) {
            wm_damage(win->x - WM_SHADOW_SIZE, win->y - WM_SHADOW_SIZE,
                      win->w + 2*WM_SHADOW_SIZE, win->h + 2*WM_SHADOW_SIZE);
        }

        win->dirty = false;
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
            if (dx*dx + dy*dy <= btn_r2) { CloseWindow(top_id); return true; }
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
            win->input(win, 1, mx - win->content_x, my - win->content_y);
        }
        return true;
    }
    wm_input_capture_id = -1;
    return false;
}

void WindowManager::HandleMouseMove(int mx, int my) {
    if (!mouse_is_down || current_action == WM_NONE) return;
    Window* win = GetWindow(action_window_id);
    if (!win) { current_action = WM_NONE; action_window_id = -1; return; }

    unsigned int now = Timer::GetRealMs();
    if (current_action == WM_DRAG && wm_drag_min_gap_ms > 0
        && (now - wm_last_drag_ms) < wm_drag_min_gap_ms) {
        return;
    }
    wm_last_drag_ms = now;

    int ox = win->x, oy = win->y, ow = win->w, oh = win->h;
    if (wm_apply_action(win, current_action, mx, my,
                        drag_offset_x, drag_offset_y)) {
        wm_damage(ox - WM_SHADOW_SIZE, oy - WM_SHADOW_SIZE,
                  ow + 2*WM_SHADOW_SIZE, oh + 2*WM_SHADOW_SIZE);
        wm_damage(win->x - WM_SHADOW_SIZE, win->y - WM_SHADOW_SIZE,
                  win->w + 2*WM_SHADOW_SIZE, win->h + 2*WM_SHADOW_SIZE);
        UpdateContentArea(win);
        win->dirty = true;
    }
}

void WindowManager::HandleMouseUp(int mx, int my) {
    // edge-snap: if the user released a drag near a screen edge, snap.
    if (current_action == WM_DRAG) {
        Window* win = GetWindow(action_window_id);
        if (win && win->resizable) {
            const int EDGE = 8;
            int sw = screen_width;
            int dy0 = wm_desktop_y;
            int dh  = wm_desktop_h;
            bool snapped = false;
            if (my <= EDGE) {
                // top -> maximize
                wm_damage_window(win);
                if (win->state != WIN_MAXIMIZED) {
                    win->saved_x = win->x; win->saved_y = win->y;
                    win->saved_w = win->w; win->saved_h = win->h;
                }
                win->x = 0; win->y = dy0;
                win->w = sw; win->h = dh;
                win->state = WIN_MAXIMIZED;
                snapped = true;
            } else if (mx <= EDGE) {
                wm_damage_window(win);
                if (win->state != WIN_MAXIMIZED) {
                    win->saved_x = win->x; win->saved_y = win->y;
                    win->saved_w = win->w; win->saved_h = win->h;
                }
                win->x = 0; win->y = dy0;
                win->w = sw / 2; win->h = dh;
                win->state = WIN_NORMAL;
                snapped = true;
            } else if (mx >= sw - EDGE) {
                wm_damage_window(win);
                if (win->state != WIN_MAXIMIZED) {
                    win->saved_x = win->x; win->saved_y = win->y;
                    win->saved_w = win->w; win->saved_h = win->h;
                }
                win->x = sw / 2; win->y = dy0;
                win->w = sw - sw / 2; win->h = dh;
                win->state = WIN_NORMAL;
                snapped = true;
            }
            if (snapped) {
                UpdateContentArea(win);
                wm_damage(win->x, win->y, win->w, win->h);
                win->dirty = true;
            }
        }
    } else if (current_action != WM_NONE) {
        Window* win = GetWindow(action_window_id);
        if (win) win->dirty = true;
    }
    mouse_is_down = false;
    current_action = WM_NONE;
    action_window_id = -1;
    (void)mx; (void)my;
}

void WindowManager::HandlePointerMove(int mx, int my) {
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

    win->input(win, 5, mx - win->content_x, my - win->content_y);
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

    win->input(win, 6, button, pressed ? 1 : 0);

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

WMAction WindowManager::GetCurrentAction() {
    return current_action;
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
void WindowManager::TickAnimations() {
    unsigned int now = Timer::GetRealMs();
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        Window* w = &windows[i];
        if (w->state == WIN_CLOSED) continue;
        if (w->anim_kind == 0) continue;
        if (w->anim_duration_ms != 0 &&
            (now - w->anim_start_ms) < w->anim_duration_ms) {
            continue;
        }
        // animation done  -  finalise according to kind
        unsigned char k = w->anim_kind;
        w->anim_kind = 0;
        switch (k) {
            case 1:  /* open  */ break;          // already visible
            case 2:  /* close */
                wm_damage_window(w);
                w->state = WIN_CLOSED;
                w->visible = false;
                w->had_last = false;
                if (focused_id == w->id) focused_id = -1;
                if (wm_input_capture_id == w->id) wm_input_capture_id = -1;
                if (action_window_id == w->id) {
                    action_window_id = -1;
                    current_action = WM_NONE;
                }
                break;
            case 3:  /* minimize */
                wm_damage_window(w);
                w->state = WIN_MINIMIZED;
                w->visible = false;
                w->had_last = false;
                break;
            case 4:  /* restore */ break;
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
    comp_anim_duration_ms     = UIConfig::Int ("compositor.animation_speed_ms",   90);
    comp_default_alpha        = UIConfig::Int ("compositor.window_alpha",         255);
    comp_frosted_titlebar     = UIConfig::Bool("compositor.frosted_titlebar",     true);
    comp_reduced_motion       = UIConfig::Bool("compositor.reduced_motion",       false);

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
    int refresh_hz = UIConfig::Int("display.refresh_hz", 60);
    bool vsync     = UIConfig::Bool("display.vsync",     true);
    if (refresh_hz < 24)  refresh_hz = 24;
    if (refresh_hz > 360) refresh_hz = 360;
    if (vsync) {
        Graphics::SetTargetFPS((uint32_t)refresh_hz);
    } else {
        Graphics::SetTargetFPS(360);
    }
    // Drag throttle: 1 frame interval  -  coalesces multi-event-per-frame
    // pointer storms into a single window move so we never repaint twice
    // for the same monitor scan-out.
    wm_drag_min_gap_ms = (unsigned int)(1000 / (refresh_hz > 0 ? refresh_hz : 60));
    if (wm_drag_min_gap_ms < 2)  wm_drag_min_gap_ms = 2;
    if (wm_drag_min_gap_ms > 16) wm_drag_min_gap_ms = 16;
    (void)comp_frosted_titlebar; // currently passive  -  read by titlebar render
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
